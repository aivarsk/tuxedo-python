#!/usr/bin/env python3

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
