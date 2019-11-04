#!/usr/bin/env python3
# HTTP+XML client as Oracle Tuxedo server
# Caches rates in memory until client.py calls RELOAD_* services

import os
import sys
import urllib.request
from xml.etree import ElementTree as et

import tuxedo as t

class Server:
    def tpsvrinit(self, args):
        self._rates = None
        t.userlog('Server startup')
        t.tpadvertise('GETRATE')
        t.tpadvertisex('RELOAD_' + str(os.getpid()), 'RELOAD',  t.TPSINGLETON + t.TPSECONDARYRQ)
        return 0

    def tpsvrdone(self):
        t.userlog('Server shutdown')

    def GETRATE(self, args):
        if self._rates is None:
            t.userlog('Loading rates')
            f = urllib.request.urlopen('https://www.ecb.europa.eu/stats/eurofxref/eurofxref-daily.xml')
            x = et.fromstring(f.read().decode('utf8'))
            self._rates = {}
            for r in x.findall('.//*[@currency]'):
                self._rates[r.attrib['currency']] = float(r.attrib['rate'])
        
        return t.tpreturn(t.TPSUCCESS, 0, {'RATE': self._rates[args['CURRENCY'][0]]})

    def RELOAD(self, args):
        t.userlog('Singleton called with' + str(args))
        self._rates = None
        return t.tpreturn(t.TPSUCCESS, 0, {})

if __name__ == '__main__':
    t.run(Server(), sys.argv)
