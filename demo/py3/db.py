#!/usr/bin/env python3

import sys
import tuxedo as t
import cx_Oracle

class Server:
    def tpsvrinit(self, args):
        t.userlog('Server startup')
        self.db = cx_Oracle.connect(handle=t.xaoSvcCtx())
        t.tpadvertise('DB')
        return 0

    def DB(self, args):
        dbc = self.db.cursor()
        dbc.execute('insert into pymsg(msg) values (:1)', ['Hello from python'])
        return t.tpreturn(t.TPSUCCESS, 0, args)

if __name__ == '__main__':
    t.run(Server(), sys.argv, 'Oracle_XA')
