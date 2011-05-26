#! /usr/bin/env python

import logging
from Identifier import Identifier
from Resource import ResourceProxy
from Resource import BasicResource, Resource
from exc import PTMException, InstanceNotFound, ConfigurationAttributeError
from abc import ABCMeta, abstractmethod

logger = logging.getLogger("ptm")

RAND_CHARS = None
choice = None

class AbstractResourceAdapter(object):
	__metaclass__ = ABCMeta
	
	#TODO: This does not belong here
	def get_client(self):
		#raise NotImplementedError()
		return self.adapter.client
	def __get_client(self):
		return self.get_client()
	client = property(__get_client)
	
	def get_logger(self):
		return logger
	
	def __get_logger(self):
		return self.get_logger()
	logger = property(__get_logger)

	@abstractmethod
	def add_resource(self, parent_id, name, typename, config, owner = None):
		raise NotImplementedError()

	@abstractmethod
	def get_resource(self, identifier):
		raise NotImplementedError()

	@abstractmethod
	def have_resource(self, identifier):
		raise NotImplementedError()
	
	@abstractmethod
	def list_resources(self, parent_id, type = None):
		raise NotImplementedError()

	@abstractmethod
	def get_configuration(self, identifier):
		raise NotImplementedError()

	@abstractmethod
	def set_configuration(self, identifier, config):
		raise NotImplementedError()

	@abstractmethod
	def get_attribute(self, identifier, name):
		raise NotImplementedError()

	@abstractmethod
	def set_attribute(self, identifier, name, value):
		raise NotImplementedError()

	@abstractmethod
	def delete_resource(self, identifier):
		raise NotImplementedError()

	def notify(self, condition, owner, ref):
		return

class BasicResourceAdapter(AbstractResourceAdapter):
	def __init__(self, manager, parent = None, *args, **kw):
		super(BasicResourceAdapter, self).__init__(*args, **kw)
		self._manager = manager

		if parent is not None:
			self.__parent_id = Identifier(parent, need_full = True)
		else:
			self.__parent_id = None

	def _mangle_id(self, identifier):
		if self.parent_id is None:
			return Identifier(Identifier.SEPARATOR).relpath_to(identifier)
		identifier =  self.parent_id.relpath_to(identifier)
			
		if identifier.is_manager:
			identifier = identifier.parent
		return identifier

	def get_manager(self):
		return self._manager
	manager = property(get_manager)

	def get_client(self):
		return self.manager.client

	def get_parent_id(self):
		return self.__parent_id
	parent_id = property(get_parent_id)

	def get_base_id(self):
		return self.parent_id and self.parent_id.submanager or Identifier(Identifier.SEPARATOR)
	base_id = property(get_base_id)

	def get_parent(self):
		return self.__parent_id and self.client.get_resource(self.__parent_id) or None
	parent = property(get_parent)

	def add_resource(self, parentId, name, typename, config, owner = None):
		parentId = self._mangle_id(parentId)
#		name = config.get("name", name)
#		config["name"] = name
		e = self._add_resource(parentId, name, typename, config)
		if e is None:
			raise PTMException("Error adding resource. RA returned None. Most likely a bug in the RA. Assuming that add failed.")
		logger.debug("Got a resource: %s" % (e, ))
		self.acquire_resource(e, owner, False)
		if not isinstance(e, Resource):
			e = Identifier(e, need_name = True)
			if not e.is_absolute:
				e = Identifier(self.parent_id) / e
			e = ResourceProxy(adapter = self.client.get_adapter(e), identifier = e)
		return e

	def _add_resource(self, parentId, name, typename, config):
		raise NotImplementedError()

	def get_resource(self, identifier):
		logger.debug("RA.get_resource %s", (identifier, ))
		mangled = self._mangle_id(identifier)
		logger.debug("mangled %s", (identifier, ))
		if not mangled:
			raise ValueError("I need a full id, not %s" % (identifier, ))
		e =  self._get_resource(mangled)
		if e is None:
			raise Exception("Adapter returned nothing for %s", identifier)
		return e

	def _get_resource(self, identifier):
		raise NotImplementedError()

	def check_resource(self, identifier):
		identifier = self._mangle_id(identifier)
		if not isinstance(identifier, BasicResource):
			try:
				self._check_resource(identifier)
			except InstanceNotFound:
				raise
			except:
				logger.exception("Error while checking resource instance %s" % (identifier, ))
				raise

	def _check_resource(self, identifier):
		self.get_resource(identifier)

	def have_resource(self, identifier):
		identifier = self._mangle_id(identifier)
		return self._have_resource(identifier)

	def _have_resource(self, identifier):
		try:
			self.check_resource(identifier)
			return True
		except:
			return False

	def list_resources(self, parent_id, type = None):
		parent_id = self._mangle_id(parent_id)
		return self._list_resources(parent_id, type)

	def _list_resources(self, parent_id, type = None):
		raise NotImplementedError()

	def get_configuration(self, identifier):
		identifier = Identifier(identifier, need_full = True)
		return self._get_configuration(identifier)
	
	def _get_configuration(self, identifier):
		return self.get_resource(identifier)._get_configuration()

	def set_configuration(self, identifier, config):
		identifier = Identifier(identifier, need_full = True)
		return self._set_configuration(identifier, config and config or {})

	def _set_configuration(self, identifier, config):
		for k, v in config.iteritems():
			self.set_attribute(identifier, k, v)

	def get_attribute(self, identifier, name):
		identifier = Identifier(identifier, need_full = True)
		return self._get_attribute(identifier, name)

	def _get_attribute(self, identifier, name):
		try:
			return self.get_configuration(identifier)[name]
		except KeyError:
			raise ConfigurationAttributeError(name)

	def set_attribute(self, identifier, name, value):
		identifier = Identifier(identifier, need_full = True)
		return self._set_attribute(identifier, name, value)

	def _set_attribute(self, identifier, name, value):
		return self.get_resource(identifier)._set_attribute(name, value)

	def delete_resource(self, identifier):
		identifier = self._mangle_id(identifier)
		self._do_delete(identifier)

	def _do_delete(self, identifier):
		assert(isinstance(identifier, Identifier))

		self._delete_children(identifier)
		self._delete_resource(identifier)


	def _delete_children(self, identifier):
		try:
			for c in self.client.list_entities(identifier):
				try:
					c.delete()
				except:
					logger.exception("Error while deleting %s (error while deleting child %s)" % (identifier, c, ))
					raise
		except:
			logger.exception("Error while deleting %s (error while listing children)", (identifier, ))
			raise

	def _delete_resource(self, identifier):
		raise NotImplementedError()

	def generate_name(self, parent_id = None, typename = None):
		global choice
		global RAND_CHARS
		if choice is None:
			from random import choice
			import string
			RAND_CHARS = string.letters + string.digits + "_"
		return ''.join([ choice(RAND_CHARS) for _ in range(16)])

	def _can_be_responsible_for(self, identifier):
		identifier = Identifier(identifier)
		return self.parent_id is None or not identifier.is_absolute or identifier.dirname == self.parent_id.submanager.dirname or identifier.identifier.startswith(self.parent_id.submanager)

	def register(self, identifier):
		identifier = Identifier(identifier, need_name = False)
		if identifier.is_absolute:
			identifier = Identifier(self.parent_id).relpath_to(identifier)
		identifier = self.parent_id / identifier

		self.manager.register_adapter(identifier, self)

class DelegateMixin(object):
	def _delete_resource(self, identifier, force):
		self.get_resource(identifier).delete()

from helpers import is_iterable

class MangleConfigMixin(object):
	def __init__(self, keys, prune = False, *args, **kw):
		super(MangleConfigMixin, self).__init__(*args, **kw)
		if isinstance(keys, basestring) or not is_iterable(keys):
			keys = (keys, )

		self.__keys = keys
		self.__prune = prune

        def add_resource(self, parentId, name, typename, config):
                config = self.mangle_config(config)
                return super(MangleConfigMixin, self).add_resource(config = config, parentId = parentId, name = name, typename = typename)

        def mangle_config(self, config):
                for k in self.__keys:
                        if k not in config:
                                raise KeyError("Missing configuration key '%s' in: %s" % (k, config))

                if self.__prune:
                        for k in config.keys():
                                if k not in self.__keys:
                                        logger.warn("Discarding configuration key: " + str(k))
                                        del config[k]

                return config

		
class AbsoluteParentMixin(object):
	def _mangle_id(self, identifier):
		identifier = super(AbsoluteParentMixin, self)._mangle_id(identifier)
		identifier = self.base_id / identifier
		if identifier.is_manager: 
			identifier = identifier.parent
		if not identifier:
			return None
		return identifier


class ReferenceDatabaseMixin(object):
	def __init__(self, *args, **kw):
		super(ReferenceDatabaseMixin, self).__init__(*args, **kw)
		from threading import Lock
		self.__lock = Lock()

	def __get_db(self):
		import shelve
		return shelve.open("/var/lib/ptm/references.db")

class AbstractGetReferencesMixin(object):
	def _get_references(self, identifier):
		raise NotImplementedError()

class ReflectiveGetReferencesMixin(AbstractGetReferencesMixin):
	def _get_references(self, identifier):
		return [ v for v in  self._get_resource(identifier).__dict__.itervalues() if isinstance(v, Resource) ]

class GrabReferencesMixin(AbstractGetReferencesMixin):
	def _add_resource(self, *args, **kw):
		e = super(GrabReferencesMixin, self)._add_resource(*args, **kw)
		for r in self._get_references(e):
			r.acquire(e)

	

class ReleaseReferencesMixin(AbstractGetReferencesMixin):
	def _delete_resource(self, identifier):
		refs = self._get_references(identifier)
		super(ReleaseReferencesMixin, self)._delete_resource(identifier)
		for r in refs:
			try:
				self.client.get_resource(r).release(identifier)
			except:
				logger.exception("Error while releasing reference: %s" % (r, ))

class DeleteUnusedMixin(object):
	def _acquire_resource(self, identifier, owner, weak = False):
		super(DeleteUnusedMixin, self)._acquire_resource(identifier, owner, weak)
		self._consider_deletion(identifier)

	def _release_resource(self, identifier, owner):
		super(DeleteUnusedMixin, self)._release_resource(identifier, owner)
		self._consider_deletion(identifier)

	def _consider_deletion(self, identifier):
		if not self._has_a_purpose(identifier):
			self._do_delete(identifier)

	def _has_a_purpose(self, identifier):
		try:
			return self.get_owners(identifier) or self._has_children_with_purpose(identifier)
		except:
			logger.exception("Error while checking purpose of %s. Depressed now. No purpose" % (identifier, ))

		return False

	def _has_children_with_purpose(self, identifier):
		children = [ c.identifier for c in self.client.list_entities(identifier) ]
#		refs = [ Identifier(r) for r in self._get_references(identifier) ]
#		for r in refs:
#			assert(r != identifier)
#			if r.startswith(identifier) and len(self.client.get_resource(r).get_strong_owners()) == 1:
#				child = identifier / (identifier.relpath_to(r)[0])
#				if child == r:
					
		return len(children) > 0

class ResourceAdapter(DelegateMixin, BasicResourceAdapter):
	pass
			
getargspec = None

class ReflectiveResourceAdapterBase(AbsoluteParentMixin, ResourceAdapter):
	def __init__(self, manager, types = None, parent = None, *args, **kw):
		ResourceAdapter.__init__(self, parent = parent, manager = manager, *args, **kw)
		self.__types = {}
		if isinstance(types, dict):
			for id, o in types.iteritems():
				self.__add_object(o, id)
		elif isinstance(types, (tuple, list, set, frozenset)):
			for k in types:
				self.__add_object(k, None)

	def __add_object(self, klass, id = False):
		if isinstance(klass, (tuple, list, set, frozenset)):
			name = klass[0]
			klass = klass[1]
		else:
			try:
				name = klass.get_typename()
			except AttributeError:
				name = klass.__name__.lower()

		if id is None or id is True:
			id = Identifier(name)
		elif id is not False:
			id = self.parent_id.relpath_to(Identifier(id, need_name = False))
	
			if id.managed_type and id.managed_type != name and not id.is_wildcard:
				raise Exception("Typename mismatch: %s <--> %s" % (name, id))

		return self._add_type(id, name, klass)

	def add_type(self, klass, name = None, id = True):
		if name is not None:
			return self.__add_object((name, klass), id)
		return self.__add_object(klass, id)

	def _add_type(self, identifier, name, klass):
		if not issubclass(klass, Resource):
			raise TypeError("Need a subclass of Resource, not " + str(klass))
		if not name:
			raise ValueError("No typename given")
#		name = Identifier(name)

		if name in self.__types:
			raise Exception("typename already present: " + name)

		self.__types[name] = klass

		if identifier is not False:
			self.register(identifier)

	def get_type(self, name):
		try:
			return self.__types[name]
		except KeyError:
			logger.exception(self.__types)
			raise Exception("Type " + str(name) + " unknown here")

	def get_types(self):
		return set(self.__types.values())
	types = property(get_types)

	def check_type(self, type):
		if type not in self.__types:
			raise Exception("No such type here: " + str(type))

	def _has_staticmethod(self, klass, name):
		return hasattr(klass, name)

	__MethodType = None

	@staticmethod
	def _import():
		global getargspec
		if getargspec is None:

			import inspect
			import types
			getargspec = inspect.getargspec
			ReflectiveResourceAdapterBase.__MethodType = types.MethodType

	def _get_parameters(self, klass, name = "__init__", fullinfo = False):
		self._import()
			
		vars = {}
		ctor = getattr(klass, name)
		signature = (False, False, False)
		if type(ctor) == ReflectiveResourceAdapterBase.__MethodType:
			signature = getargspec(ctor)
			params = signature[0]
			defaults = signature[-1] or ()
			if len(params) == 0:
				raise Exception(klass.__name__ + "." + name + " takes no arguments, not even self")
			params = params[1:]
			for i, p in enumerate(params, 1):
				vars[p] = len(defaults) - (len(params) - i) > 0

		if fullinfo:
			return (vars, signature[2])
		return vars

	def _check_params(self, klass, name, strict = False, needle = ("identifier", "adapter")):
		info = self._get_parameters(klass, name, True)
		params = info[0]

		if not info[-1]:
			for n in needle:
				if n not in params:
					raise TypeError(klass.__name__ + "." + name + " must accept '" + n + "'")

		if strict:
			for k, optional in params.iteritems():
				if k not in needle and not optional:
					raise TypeError(klass.__name__ + "." + name + " must not enforce " + str(k))

class GetParentMixin(AbsoluteParentMixin):
	def _mangle_id(self, identifier):
		identifier = super(GetParentMixin, self)._mangle_id(self, identifier)
		if identifier:
			return self.client.get_resource(identifier)
		return None


class ReflectiveAddAdapterBase(ReflectiveResourceAdapterBase):
	def _mangle_args(self, klass, kwargs, name = "__init__"):
		info = self._get_parameters(klass, fullinfo = True, name = name)
		params = info[0]

		params.pop("adapter", None)
		params.pop("identifier", None)
		params.pop("name", None)
		params.pop("parent", None)

		if not info[-1]:
			for k in kwargs.keys():
				if k not in params:
					logger.warning("Ignoring configuration key: " + k)
					del kwargs[k]
		#missing = 0
		for p, optional in params.iteritems():
			if not optional and p not in kwargs:
#				if not args:
					raise Exception("Missing configuration key: " + p)
#				missing += 1
		
#		if missing > len(args):
#			raise Exception("Not enough arguments")
		return kwargs

	def _mangle_name(self, parentId, name, klass):
		if not name:
			try:
				return klass.generate_name(parentId)
			except AttributeError:
				name = self.generate_name(parentId, klass.get_typename())

		return name


class ReflectiveAddAdapter(ReflectiveAddAdapterBase):
	def _add_type(self, identifier, name, klass):
		self._check_params(klass, "add_instance", needle = ("parent", "adapter", "name"))
		return super(ReflectiveAddAdapter, self)._add_type(identifier, name, klass)

	def _add_resource(self, parentId, name, typename, kw):
		klass = self.get_type(typename)
#		name = self._mangle_name(parentId, name, klass)
		kw = self._mangle_args(klass, kw)
		return klass.add_instance(parent = parentId, adapter = self, name = name, **kw)

class ReflectiveConstructorAddAdapter(ReflectiveAddAdapterBase):
	def _add_type(self, identifier, name, klass):
		self._check_params(klass, "__init__", needle = ("parent", "adapter", "name"))
		return super(ReflectiveConstructorAddAdapter, self)._add_type(identifier, name, klass)

	def _add_resource(self, parentId, name, typename, kw):
		klass = self.get_type(typename)
#		name = self._mangle_name(name, klass)
		kw = self._mangle_args(klass, kw)
		return klass(parent = parentId, name = name, adapter = self, **kw)

class ReflectiveGetAdapter(ReflectiveResourceAdapterBase):
	def _add_type(self, identifier, name, klass):
		self._check_params(klass, "get_instance", True, needle = ("parent", "adapter", "name"))
		return super(ReflectiveGetAdapter, self)._add_type(identifier, name, klass)

	def _get_resource(self, identifier):
		klass = self.get_type(identifier.typename)
		parent = identifier.parent
		if not parent:
			parent = None
		return klass.get_instance(parent = parent, name = identifier.resourcename, adapter = self)

class ReflectiveConstructorGetAdapter(ReflectiveResourceAdapterBase):
	def _add_type(self, identifier, name, klass):
		self._check_params(klass, "__init__", True, needle = ("parent", "adapter", "name"))
		return super(ReflectiveConstructorGetAdapter, self)._add_type(identifier, name, klass)

	def _get_resource(self, identifier):
		klass = self.get_type(identifier.typename)
		parent = identifier.parent
		if not parent:
			parent = None
		return klass(parent = parent, name = identifier.resourcename, adapter = self)

class ReflectiveListAdapter(ReflectiveResourceAdapterBase):
	def _add_type(self, identifier, name, klass):
		self._check_params(klass, "list_instances", True, needle = ("parent", "adapter"))
		return super(ReflectiveListAdapter, self)._add_type(identifier, name, klass)

	def _list_entities(self, parent_id, typename = None):
		if typename:
			return self.get_type(typename).list_instances(parent = parent_id, adapter = self)
		return reduce(list.__add__, [ [ i for i in klass.list_instances(parent = parent_id, adapter = self) if i.typename == klass.get_typename() ] for klass in self.types] , [])

class ReflectiveResourceAdapter(ReflectiveGetAdapter, ReflectiveListAdapter, ReflectiveAddAdapter):
	pass

