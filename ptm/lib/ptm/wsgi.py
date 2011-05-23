#! /usr/bin/env python

from SimpleXMLRPCServer import SimpleXMLRPCDispatcher
from SocketServer import ThreadingMixIn
from wsgiref.simple_server import WSGIServer

class ThreadingWSGIServer(ThreadingMixIn, WSGIServer):
	pass

class WSGIXMLRPCRequestHandler(SimpleXMLRPCDispatcher):
	def __init__(self, encoding=None):
		SimpleXMLRPCDispatcher.__init__(self, allow_none = True, encoding = encoding)

	def get_logger(self):
		try:
			return self.__logger
		except AttributeError:
			self.__logger = self._get_logger()
			return self.__logger
	logger = property(get_logger)
	
	def _get_logger(self):
		import logging
		return logging

	def __call__(self, environ, start_response):
		if environ["REQUEST_METHOD"] != "POST":
			headers = [("Content-type", "text/html")]

			if environ["REQUEST_METHOD"] == "HEAD":
				data = ""
			else:
				data = "<html><head><title>400 Bad request</title></head><body><h1>400 Bad request</h1></body></html>"
			headers.append(("Content-length", str(len(data))))
			start_response("400 Bad request", headers)
			return (data, )

		l = int(environ["CONTENT_LENGTH"])
		request = environ["wsgi.input"].read(l)
		response = self._marshaled_dispatch(request)
		headers = [("Content-type", "text/xml"), ("Content-length", str(len(response)))]
		start_response("200 OK", headers)
		return (response, )

	def _dispatch(self, *args, **kw):
		#self.logger.debug(args)
		#self.logger.debug(kw)
		try:
			result = SimpleXMLRPCDispatcher._dispatch(self, *args, **kw)
		#	self.logger.debug("Result: %s" % (result, ))
			return result
		except:
			self.logger.exception("Error while processing request")
			raise
	
from xmlrpclib import Fault
from exc import PTMException, convert_exception

class ManagerWSGIXMLRPCRequestHandler(WSGIXMLRPCRequestHandler):
	def _get_logger(self):
		import logging
		return logging.getLogger("ptm")
	
	def _dispatch(self, *args, **kw):
		try:
			return  WSGIXMLRPCRequestHandler._dispatch(self, *args, **kw)
		except PTMException, e:
			raise Fault(faultCode = e.code, faultString = e.message)
		except Exception, e:
			e = convert_exception(e)
			raise Fault(faultCode = e.code, faultString = e.message)
		