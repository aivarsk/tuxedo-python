#!/usr/bin/env python

import tuxedo as t

if __name__ == '__main__':
    buf = {'TA_CLASS': ['T_SVCGRP'], 'TA_OPERATION': ['GET']}

    assert t.tpimport(t.tpexport(buf)) == buf
    assert t.tpimport(t.tpexport(buf, t.TPEX_STRING), t.TPEX_STRING) == buf

    assert t.Fname32(t.Fldid32('TA_OPERATION')) == 'TA_OPERATION'

    assert t.Fldtype32(t.Fmkfldid32(t.FLD_STRING, 10)) == t.FLD_STRING
    assert t.Fldno32(t.Fmkfldid32(t.FLD_STRING, 10)) == 10

    binstr =  b'\xc1 hello'
    binstr2 = t.tpimport(t.tpexport({'TA_OPERATION': binstr}))['TA_OPERATION'][0]
    assert binstr2.encode(errors='surrogateescape') == binstr
    t.tpexport({'TA_OPERATION': binstr2})
    binstr3 = t.tpimport(t.tpexport({'TA_OPERATION': binstr2}))['TA_OPERATION'][0]
    assert binstr3.encode(errors='surrogateescape') == binstr

    utf8 = b'gl\xc4\x81\xc5\xbe\xc5\xa1\xc4\xb7\xc5\xab\xc5\x86r\xc5\xab\xc4\xb7\xc4\xabtis'
    s = t.tpimport(t.tpexport({'TA_OPERATION': utf8}))['TA_OPERATION'][0]
    assert s.encode('utf8') == utf8

    uni = 'gl\u0101\u017e\u0161\u0137\u016b\u0146r\u016b\u0137\u012btis'
    s = t.tpimport(t.tpexport({'TA_OPERATION': uni}))['TA_OPERATION'][0]
    assert s == uni

    assert t.Fboolev32({'TA_OPERATION': '123456789'}, "TA_OPERATION=='123456789'")
    assert not t.Fboolev32({'TA_OPERATION': '123456789'}, "TA_OPERATION=='1234567890'")
    assert t.Fboolev32({'TA_OPERATION': '123456789'}, "TA_OPERATION%%'.234.*'")
    assert not t.Fboolev32({'TA_OPERATION': '123456789'}, "TA_OPERATION%%'.123.*'")
    assert t.Fboolev32({'TA_OPERATION': '123456789'}, "TA_OPERATION!%'.123.*'")

    import sys
    t.Ffprint32({'TA_OPERATION': '123456789'}, sys.stdout)

    t.Ffprint32({t.Fmkfldid32(t.FLD_STRING, 10): 'Dynamic field'}, sys.stdout)

    print(t.Fextread32(sys.stdin))
