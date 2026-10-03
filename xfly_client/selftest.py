"""协议层自测：`python main.py --selftest`。

覆盖：PBH 编码、各 FSD 包构造/解析、频率换算、认证算法确定性。
"""

from __future__ import annotations

from .fsd import packet as pk
from .fsd import parser as ps
from .fsd.auth import FsdAuthState


def run_all() -> int:
    fails = 0
    results: list[tuple[bool, str]] = []

    def check(cond: bool, name: str) -> None:
        results.append((cond, name))
        nonlocal fails
        if not cond:
            fails += 1

    # --- PBH 编码（swift 公式：有符号取反 + 1024 量化） ---
    check(pk.encode_pbh(0, 0, 0) == 0, "PBH zero")
    pbh = pk.encode_pbh(10, -20, 90)
    check(((pbh >> 22) & 0x3FF) == ((-28) & 0x3FF), "PBH pitch field (10deg)")
    check(((pbh >> 12) & 0x3FF) == (56 & 0x3FF), "PBH bank field (-20deg)")
    check(((pbh >> 2) & 0x3FF) == 256, "PBH heading field (90deg)")
    check((pk.encode_pbh(0, 0, 0) & 0b10) == 0, "PBH on_ground clear (default)")
    check((pk.encode_pbh(0, 0, 0, on_ground=True) & 0b10) == 0b10, "PBH on_ground set")
    pbh_g = pk.encode_pbh(10, -20, 90, on_ground=True)
    check(((pbh_g & 0b10) >> 1) == 1 and ((pbh_g >> 22) & 0x3FF) == ((-28) & 0x3FF),
          "PBH on_ground coexists with angles")
    # 解码往返
    h, b, p, g = pk.decode_pbh(pk.encode_pbh(10, -20, 90, True))
    check(abs(h - 90) < 0.6 and abs(b - (-20)) < 1.5 and abs(p - 10) < 1.5
          and g, "PBH decode round-trip")

    # --- 包构造 ---
    ap = pk.add_pilot("DAL625", "1400000", "secret", 1, 9, 16, "Zhang San")
    check(ap == "#APDAL625:SERVER:1400000:secret:1:9:16:Zhang San", "#AP layout")
    pos = pk.pilot_position("DAL625", "N", "2000", 1, 40.65906, -73.79891,
                            26000, 400, pk.encode_pbh(0, 0, 90), 359)
    check(pos.startswith("@N:DAL625:2000:1:40.65906:-73.79891:26000:400:"),
          "pilot pos layout")
    check(pk.delete_pilot("AAL325", "1400000") == "#DPAAL325:1400000", "#DP layout")
    check(pk.text_message("N7938C", "@22800", "hello") ==
          "#TMN7938C:@22800:hello", "#TM layout")
    check(pk.metar_request("ZGGG_GND", "ZGGG") ==
          "$AXZGGG_GND:SERVER:METAR:ZGGG", "$AX layout")
    check(pk.plane_info_request("CCA987", "CSN3081") == "#SBCCA987:CSN3081:PIR",
          "PIR layout")
    check(pk.plane_info_fsi("CCA987", "CSN3081", "B738", "CSN",
                            "CSN B738 B-1234") ==
          "#SBCCA987:CSN3081:FSIPI:0:CSN:B738::::::CSN B738 B-1234",
          "FSIPI layout")
    fp = pk.flight_plan("CCA1234", "I", "A20N", "450", "ZGGG", "1200", "1200",
                        "FL350", "ZBAD", 2, 30, 5, 20, "ZSNJ",
                        "RMK:POB10", "DCT SHL W22 TYN B208")
    check(fp == ("$FPCCA1234:SERVER:I:A20N:450:ZGGG:1200:1200:FL350:ZBAD:"
                 "2:30:5:20:ZSNJ:RMKPOB10:DCT SHL W22 TYN B208"),
          "$FP layout (colon removed)")

    # --- 解析器 ---
    parsed = ps.parse("@S:GTI8197:2000:1:40.65906:-73.79891:26:0:4290776072:359")
    check(parsed.kind == "@" and parsed.from_ == "GTI8197", "parse @ from")
    pp = ps.try_get_pilot_position(parsed)
    check(pp is not None and pp.callsign == "GTI8197"
          and abs(pp.lat - 40.65906) < 1e-5 and pp.alt_ft == 26, "parse @ fields")

    atc = ps.parse("%EWR_P_APP:28550:5:150:4:40.67317:-74.18533:0")
    a2 = ps.try_get_atc_position(atc)
    check(a2 is not None and a2.callsign == "EWR_P_APP" and a2.freq_khz == 28550
          and a2.facility == 5 and abs(a2.lon - (-74.18533)) < 1e-5, "parse % fields")

    er = ps.parse("$ERSERVER:unknown:006::Invalid CID/password.")
    check(er.kind == "$ER" and er.type == "006" and len(er.flds) == 2
          and er.flds[1] == "Invalid CID/password.", "parse $ER")

    ar = ps.parse("$ARSERVER:ZGGG_GND:ZGGG 041600Z 05003MPS CAVOK 22/20 Q1009 NOSIG $")
    check(ar.kind == "$AR" and ar.from_ == "SERVER" and ar.to == "ZGGG_GND"
          and ar.type.startswith("ZGGG"), "parse $AR")

    ar2 = ps.parse("$ARSERVER:ZGGG_GND:METAR:ZGGG 041600Z 05003MPS CAVOK")
    check(ar2.kind == "$AR" and ar2.type == "METAR"
          and ar2.flds[0].startswith("ZGGG"), "parse $AR METAR: prefix")

    tm_in = ps.parse("#TMZGGG_TWR:CSN3081:contact delivery 121.7")
    check(tm_in.kind == "#TM" and tm_in.from_ == "ZGGG_TWR" and tm_in.to == "CSN3081"
          and tm_in.type == "contact delivery 121.7", "parse #TM")

    fast = ps.parse("^CSA1234:22.7:114.3:32060.5:1500.0:1712:0.001:0.0002:0.0005:0:0:0:-0.4")
    fp = ps.try_get_pilot_position(fast)
    check(fp is not None and fp.callsign == "CSA1234" and abs(fp.lat - 22.7) < 1e-6
          and fp.alt_ft == 32060 and fp.pbh == 1712, "parse ^ fast pos")

    slow = ps.parse("#SLCCA987:40.0:-73.0:10000:500:4290776072")
    sp = ps.try_get_pilot_position(slow)
    check(sp is not None and sp.callsign == "CCA987" and sp.pbh == 4290776072,
          "parse #SL")

    stopped = ps.parse("#STCSN3081:23.0:113.0:35.2:34.9:1024")
    stp = ps.try_get_pilot_position(stopped)
    check(stp is not None and stp.callsign == "CSN3081" and abs(stp.lat - 23.0) < 1e-6
          and stp.alt_ft == 35, "parse #ST")

    pig = ps.parse("#SBCSN3081:CCA987:PI:GEN:EQUIPMENT=B738:AIRLINE=CSN")
    check(pig.kind == "#SB" and pig.type == "PI" and pig.from_ == "CSN3081"
          and len(pig.flds) == 3 and pig.flds[0] == "GEN"
          and pig.flds[1] == "EQUIPMENT=B738", "parse PI:GEN fields")

    fsi = ps.parse("#SBCSN3081:CCA987:FSIPI:0:CSN:B738::::::CSN B738 B-1234")
    check(fsi.kind == "#SB" and fsi.type == "FSIPI" and len(fsi.flds) == 9
          and fsi.flds[2] == "B738" and fsi.flds[8] == "CSN B738 B-1234",
          "parse FSIPI fields")

    # --- 频率换算 ---
    check(pk.freq_to_target(122.800) == "@22800", "freq target 122.800")
    check(pk.freq_to_target(121.500) == "@21500", "freq target 121.500")
    check(pk.khz_to_display(22800) == "122.800", "freq display")
    check(pk.millihertz_to_display(118050) == "118.050", "millihertz display")

    # --- 认证确定性 ---
    def make_auth():
        a = FsdAuthState()
        a.init(55538, "ImuL1WbbhVuD8d3MuKpWn2rrLZRa9iVP")
        a.round_received("76617473696d")
        return a

    a1, a2 = make_auth(), make_auth()
    check(len(a1.client_challenge_key) == 32
          and a1.client_challenge_key == a2.client_challenge_key,
          "auth deterministic")
    check(a1.respond_to_challenge("0123456789abcdef")
          == a2.respond_to_challenge("0123456789abcdef"),
          "auth challenge same")

    for ok, name in results:
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
    print("ALL TESTS PASSED" if fails == 0 else f"{fails} TEST(S) FAILED")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    raise SystemExit(run_all())
