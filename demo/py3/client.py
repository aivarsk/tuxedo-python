#!/usr/bin/env python3

import tuxedo as t

if __name__ == '__main__':
    # We can call .TMIB because of tpinit(tpsysadm)
    _, _, data = t.tpcall('.TMIB', {'TA_CLASS': 'T_SVCGRP', 'TA_OPERATION': 'GET'})
    # Reset caches on ecb.py servers
    for grpno, srvid, servicename in zip(data['TA_GRPNO'], data['TA_SRVID'], data['TA_SERVICENAME']):
        if servicename.startswith('RELOAD_'):
            print('Reloading cache for srvid=%s grpno=%s' % (srvid, grpno))
            t.tpcall(servicename, {})

    # Try out in-memory cache
    t.tpcall('MEMPUT', {'KEY': 'A', 'VALUE': 'a'})
    t.tpcall('MEMPUT', {'KEY': ['B', 'C'], 'VALUE': ['b', 'c']})
    _, _, data = t.tpcall('MEMGET', {'KEY': ['B', 'C', 'A']})
    assert data['VALUE'] == ['b', 'c', 'a']
