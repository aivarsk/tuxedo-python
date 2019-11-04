#!/usr/bin/env python3
# In-memory cache, multi-threaded Oracle Tuxedo server

import sys
import tuxedo as t

class Server:
    def tpsvrinit(self, args):
        self.db = {}
        t.userlog('Server startup')
        t.tpadvertise('MEMPUT')
        t.tpadvertise('MEMGET')
        return 0

    def tpsvrthrinit(self, args):
        t.userlog('Server thread startup')
        return 0

    def tpsvrthrdone(self):
        t.userlog('Server thread shutdown')

    def tpsvrdone(self):
        t.userlog('Server shutdown')

    def MEMPUT(self, args):
        for key, value in zip(args['KEY'], args['VALUE']):
            self.db[key] = value
        return t.tpreturn(t.TPSUCCESS, 0, {})

    def MEMGET(self, args):
        args['VALUE'] = []
        for key in args['KEY']:
            args['VALUE'].append(self.db.get(key, ''))
        return t.tpreturn(t.TPSUCCESS, 0, args)

if __name__ == '__main__':
    t.run(Server(), sys.argv)
