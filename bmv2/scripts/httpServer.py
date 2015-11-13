#!/usr/bin/env python

from twisted.internet import reactor, defer
from twisted.internet.task import deferLater
from twisted.web.resource import Resource
from twisted.web.server import Site, NOT_DONE_YET
import os
import json
THIS_DIR=os.path.dirname(os.path.realpath(__file__))

class MainPage(Resource):
  def getChild(self, name, request):
    if name == '':
      return self
    else:
      return Resource.getChild(self, name, request)

  def render_GET(self, request):
    f = open('%s/web/index.html' % THIS_DIR, 'r')
    return f.read()  

class WebServer(Resource):
  isLeaf = True

  def __init__(self, proposer):
    Resource.__init__(self)
    self.proposer = proposer

  def _waitResponse(self, result, request):
    request.write(result)
    request.finish()

  def render_GET(self, request):
    data = json.dumps(request.args)
    d = self.proposer.submit(data)
    d.addCallback(self._waitResponse, request)
    return NOT_DONE_YET

  def render_POST(self, request):
    data = json.dumps(request.args)
    d = self.proposer.submit(data)
    d.addCallback(self._waitResponse, request)
    return NOT_DONE_YET

if __name__=='__main__':
    root = MainPage()
    server = WebServer()
    root.putChild('get', server)
    root.putChild('put', server)
    factory = Site(root)
    reactor.listenTCP(8080, factory)
    reactor.run()