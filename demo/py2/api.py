#!/usr/bin/env python
# HTTP+JSON API server as Oracle Tuxedo server

import os
import sys
import SimpleHTTPServer
import SocketServer
import json
import threading

import tuxedo as t

PORT = 8000

class Handler(SimpleHTTPServer.SimpleHTTPRequestHandler):
    def do_POST(self):
        content = self.rfile.read(int(self.headers.get('Content-Length', '0')))
        if content:
            t.userlog('Received ' + str(content))
            data = json.loads(content)

            for k, v in data.items():
                _, _, buf = t.tpcall('GETRATE', {'CURRENCY': k}, t.TPNOTRAN)
                data = {'EUR': str(float(v) / buf['RATE'][0])}

            content = json.dumps(data).encode('utf-8')
            t.userlog('Returning ' + str(content))

            self.send_response(200)
            self.send_header("Content-type", "application/json")
            self.end_headers()
            self.wfile.write(content)
            return
        else:
            self.send_response(500)
            self.end_headers()
            return 'no request'

def serve():
    SocketServer.ThreadingTCPServer.allow_reuse_address = True
    httpd = SocketServer.ThreadingTCPServer(("", PORT), Handler)
    t.userlog('serving at port ' + str(PORT))
    httpd.serve_forever()

class Server:
    def tpsvrinit(self, args):
        t.userlog('Server startup')
        thread = threading.Thread(target=serve)
        thread.daemon = True
        thread.start()
        return 0

    def tpsvrdone(self):
        t.userlog('Server shutdown')

if __name__ == '__main__':
    t.run(Server(), sys.argv)
