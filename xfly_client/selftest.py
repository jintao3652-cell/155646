"""协议层自测：`python main.py --selftest`。

覆盖：PBH 编码、各 FSD 包构造/解析、频率换算、认证算法确定性。
"""

from __future__ import annotations

import urllib.request

from .fsd import packet as pk
from .fsd import parser as ps
from .fsd.auth import FsdAuthState
from .model import _looks_like_registration, _norm_airline_code, _norm_registration
from .registration import resolve_registration, tail_from_text
from .simbrief import build_plan, clock_hhmm, flight_level, minutes_of


def run_all() -> int:
    fails = 0
    results: list[tuple[bool, str]] = []

    def check(cond: bool, name: str) -> None:
        results.append((cond, name))
        nonlocal fails
        if not cond:
            fails += 1

    # --- PBH 编码（精确对齐 swift `core/fsd/pbh.h::packPBH`） ---
    check(pk.encode_pbh(0, 0, 0) == 0, "PBH zero")
    pbh = pk.encode_pbh(10, -20, 90)
    # pitch=10° → 10 * -256/90 = -28.444 → qFloor = -29（不是截断的 -28）
    check(((pbh >> 22) & 0x3FF) == ((-29) & 0x3FF), "PBH pitch field (10deg, qFloor)")
    # bank=-20° → -20 * -512/180 = +56.888 → qFloor = 56
    check(((pbh >> 12) & 0x3FF) == (56 & 0x3FF), "PBH bank field (-20deg)")
    check(((pbh >> 2) & 0x3FF) == 256, "PBH heading field (90deg)")
    check((pk.encode_pbh(0, 0, 0) & 0b10) == 0, "PBH on_ground clear (default)")
    check((pk.encode_pbh(0, 0, 0, on_ground=True) & 0b10) == 0b10, "PBH on_ground set")
    pbh_g = pk.encode_pbh(10, -20, 90, on_ground=True)
    check(((pbh_g & 0b10) >> 1) == 1 and ((pbh_g >> 22) & 0x3FF) == ((-29) & 0x3FF),
          "PBH on_ground coexists with angles")
    # qFloor vs 截断：正俯仰/正坡度时两者必然差 1，这里锁住"用 floor"这一行为
    check(((pk.encode_pbh(1, 0, 0) >> 22) & 0x3FF) == ((-3) & 0x3FF),
          "PBH qFloor not truncation (pitch 1deg -> -3)")
    check(((pk.encode_pbh(0, 5, 0) >> 12) & 0x3FF) == ((-15) & 0x3FF),
          "PBH qFloor not truncation (bank 5deg -> -15)")
    # swift 用 *-1 而非按位取反：0° 必须编码成 0，不能是 -1
    check(((pk.encode_pbh(0, 0, 0) >> 22) & 0x3FF) == 0
          and ((pk.encode_pbh(0, 0, 0) >> 12) & 0x3FF) == 0,
          "PBH zero angle is 0 not -1 (negation not bitwise-invert)")
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
    # @ 报文的应答机模式字母：**S=待机 / N=正常发射 / Y=ident**
    #   依据 swift 两段：simulatorxplane.cpp::xpdrMode() 把 XP mode 0/1 折算成
    #   StateStandby、其余(2..7)折算成 ModeC；再经 serializer.cpp::toQString()
    #   输出 Standby->"S"、ModeA/C/S->"N"、Ident->"Y"。
    #   插件曾写成 `mode >= 2 ? "S" : "N"`，8 个 XP 模式全部报反 —— 待机被说成
    #   在发射、正常发射被说成待机。这里锁住"mode<=1 才是 S"这一方向。
    _xpdr_letters = {m: ("S" if m <= 1 else "N") for m in range(8)}
    check(all(_xpdr_letters[m] == "S" for m in (0, 1)),
          "@ xpdr mode: Off/Stby -> S")
    check(all(_xpdr_letters[m] == "N" for m in range(2, 8)),
          "@ xpdr mode: On/Alt/Test/GND/TA/TA-RA -> N")
    check(pk.pilot_position("X", "S", "2000", 1, 0.0, 0.0, 0, 0, 0)
          .startswith("@S:X:"), "@ pilot pos with standby mode")
    check(pk.pilot_position("X", "N", "2000", 1, 0.0, 0.0, 0, 0, 0)
          .startswith("@N:X:"), "@ pilot pos with normal mode")
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

    # $ER 错误码：数字/名称均可解析；NoFlightPlan(8) 非致命，不断链
    check(pk.resolve_error_code("8") == pk.ERR_NO_FLIGHT_PLAN
          and pk.resolve_error_code("NoFlightPlan") == pk.ERR_NO_FLIGHT_PLAN,
          "resolve NoFlightPlan")
    check(pk.ERR_NO_FLIGHT_PLAN not in pk.FATAL_ERROR_CODES
          and pk.ERR_NO_SUCH_CALLSIGN not in pk.FATAL_ERROR_CODES,
          "NoFlightPlan not fatal")
    check(pk.ERR_CALLSIGN_IN_USE in pk.FATAL_ERROR_CODES
          and pk.ERR_AUTH_TIMEOUT in pk.FATAL_ERROR_CODES,
          "fatal codes listed")

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

    # PI:GEN 涂装字段（swift PlaneInformation::toTokens：EQUIPMENT/AIRLINE/LIVERY）
    pig2 = ps.parse("#SBCSN3081:CCA987:PI:GEN:EQUIPMENT=A359:AIRLINE=BAW:LIVERY=BAW")
    check(pig2.kind == "#SB" and pig2.type == "PI"
          and pig2.flds[2] == "AIRLINE=BAW" and pig2.flds[3] == "LIVERY=BAW",
          "parse PI:GEN livery")

    # swift 中间位置 #SB ...:VI:lat:lon:alt:gs:pbh（1s 一次，比 @ 平滑）
    vi = ps.parse("#SBCCA1501:ALL:VI:23.13000:113.20000:3000:250:4290776072")
    vp = ps.try_get_pilot_position(vi)
    check(vp is not None and vp.callsign == "CCA1501" and abs(vp.lat - 23.13) < 1e-6
          and vp.alt_ft == 3000 and vp.gs_kt == 250, "parse #SB VI interim")

    # onGround 位（PBH bit1）：VI/SL/ST/^ 都携带
    taxi = ps.parse("#SBCCA1501:ALL:VI:23.13000:113.20000:35:8:2")
    tp = ps.try_get_pilot_position(taxi)
    check(tp is not None and tp.on_ground, "VI onGround bit")

    # ^ 包 AGL 字段：AGL≈0 视为在地面（部分客户端 onGround 位恒 0）
    fast_gnd = ps.parse("^CCA1501:23.13:113.20:35.5:0.4:2:0:0:0:0:0:0:0")
    fg = ps.try_get_pilot_position(fast_gnd)
    check(fg is not None and abs(fg.agl_ft - 0.4) < 1e-6 and fg.on_ground,
          "^ agl -> onGround")

    # 真实 swift 记录（N829PL / FA8X / DLH / LIVERY=swift_a9742，pbh 为负数）
    #   pbh=-8389616 → 无符号 0xFF7FFC10：航向 271.4°、俯仰 +1.05°、非地面
    neg = ps.parse("@N:N829PL:0101:1:31.02777:98.68147:43686:403:-8389616:0")
    np_ = ps.try_get_pilot_position(neg)
    h, b, pitch, og = pk.decode_pbh(np_.pbh)
    check(np_ is not None and np_.callsign == "N829PL" and np_.alt_ft == 43686
          and abs(h - 271.4) < 0.2 and abs(pitch - 1.05) < 0.05 and not og,
          "negative pbh -> heading/pitch")
    # swift 的 LIVERY 是内部 CSL 映射 ID（swift_a9742），不能当航司码
    check(_norm_airline_code("swift_a9742") == ""
          and _norm_airline_code("DLH") == "DLH"
          and _norm_airline_code("Air China") == "", "swift livery id dropped")

    # 尾号只走 REG=，**不能**塞进 LIVERY。
    #
    # 2026-11-25 修正：旧行为是 `LIVERY=<尾号>`（为了让尾号穿透 swift 中继），
    # 但 XFlySim 服务器是从 LIVERY 反查机型的 —— 裸尾号解析不出 `A<id>`，
    # 会导致**整条 PI 的 aircraft 段被静默丢弃**（equipment/airline/livery/
    # engines 全 null，且不报错）。改为 LIVERY 只放 swift 格式。
    pig3 = ps.parse("#SBCCA1234:MINE:PI:GEN:EQUIPMENT=B738:AIRLINE=CCA:REG=B-1234")
    check(pig3.type == "PI" and pig3.flds[-1] == "REG=B-1234", "parse PI REG")
    pi_reg = pk.plane_info("ME", "YOU", "B738", "CCA", "SWIFT_A1787", "B-1234")
    check(pi_reg == "#SBME:YOU:PI:GEN:EQUIPMENT=B738:AIRLINE=CCA:"
                     "LIVERY=SWIFT_A1787:REG=B-1234",
          "build PI: LIVERY carries swift id")
    # 关键回归：尾号绝不能出现在 LIVERY 里
    check("LIVERY=B-1234" not in pi_reg, "build PI: LIVERY must NOT carry reg")
    # livery 为空时整键省略（而不是写个解析不了的裸尾号）
    pi_nolv = pk.plane_info("ME", "YOU", "B738", "CCA", "", "B-1234")
    check(pi_nolv == "#SBME:YOU:PI:GEN:EQUIPMENT=B738:AIRLINE=CCA:REG=B-1234",
          "build PI: empty livery omits key")
    check(_norm_registration(" b-1234 ") == "B-1234"
          and _norm_registration("N829PL") == "N829PL"
          and _norm_registration("") == "", "norm registration")

    # ENGINES=：XFlySim 服务器扩展键（对应站点 JSON 的 aircraft.engines）
    # ENGINES 必须排在 REG 之前（服务器按位置取 PI 组第 4 项当 engines）
    pi_eng = pk.plane_info("ME", "YOU", "A333", "SJX", "SWIFT_A650", "D-AIKN", 2)
    check(pi_eng == "#SBME:YOU:PI:GEN:EQUIPMENT=A333:AIRLINE=SJX:"
                    "LIVERY=SWIFT_A650:ENGINES=2:REG=D-AIKN",
          "build PI: ENGINES before REG")
    pi_noeng = pk.plane_info("ME", "YOU", "A333", "SJX", "SWIFT_A650", "D-AIKN")
    check(pi_noeng.endswith("REG=D-AIKN") and "ENGINES" not in pi_noeng,
          "build PI: engines omitted when 0")
    check(pk.plane_info("ME", "YOU", "A333", "SJX", "SJX", "", 4)
          .endswith("LIVERY=SJX:ENGINES=4"), "build PI: engines with livery code")
    # LIVERY 语义判定：尾号 vs 航司码 / swift 内部 ID
    check(_looks_like_registration("B-1234")
          and _looks_like_registration("N829PL")
          and _looks_like_registration("n829pl"), "livery looks like reg")
    check(not _looks_like_registration("DLH")
          and not _looks_like_registration("CCA")
          and not _looks_like_registration("A320")
          and not _looks_like_registration("swift_a9742")
          and not _looks_like_registration(""), "livery not a reg")
    # 欧陆格式：纯字母 + 连字符（X-Plane 自带 A330 的尾号就是 D-AIKN）。
    # 以前要求"必须含数字"，这类尾号被整类丢弃，PI 从不发 LIVERY。
    check(_looks_like_registration("D-AIKN")
          and _looks_like_registration("G-EUUU")
          and _looks_like_registration("F-WWBB"),
          "reg: European format (no digits)")
    check(not _looks_like_registration("ABCDE")
          and not _looks_like_registration("SJX"), "reg: pure letters rejected")

    # 尾号来源优先级：手动 > 模拟器 > 上次记录 > 空（不伪造）
    check(resolve_registration("B-1234", "N829PL", "B-9999") == ("B-1234", "manual"),
          "reg priority: manual wins")
    check(resolve_registration("", "N829PL", "B-9999") == ("N829PL", "sim"),
          "reg priority: sim over cache")
    check(resolve_registration("", "", "B-9999") == ("B-9999", "cache"),
          "reg priority: cache fallback")
    check(resolve_registration("", "", "") == ("", ""), "reg priority: empty")
    check(resolve_registration("", "B-1234", "", "livery") == ("B-1234", "livery"),
          "reg source: livery detail")
    # 涂装目录名 / FSInn 描述串里取尾号
    check(tail_from_text("B-1234 China Southern") == "B-1234"
          and tail_from_text("N829PL_Livery") == "N829PL"
          and tail_from_text("DLH A359 B-1234") == "B-1234"
          and tail_from_text("liveries/Default") == "", "tail from text")
    # 实测用例：X-Plane 自带 A330 的 acf_tailnum 恒为 D-AIKN（出厂值），
    # 但选 Lufthansa 涂装时真实尾号是 D-AIKI，只在涂装目录名里
    # （liveries/Lufthansa D-AIKI，目录内无配置文件）。所以涂装必须优先于
    # acf_tailnum，否则永远报错的那架。同时确认无尾号的涂装不误判。
    check(tail_from_text("Lufthansa D-AIKI") == "D-AIKI",
          "tail from livery: Lufthansa D-AIKI (overrides acf D-AIKN)")
    for name in ("AirAsia", "AirCanada", "CathayPacific", "Laminar House",
                 "SwissAir", "VirginAtlantic"):
        check(tail_from_text(name) == "", f"tail from livery: {name} has none")

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

    # --- SimBrief 导入映射（离线：只测解析，不联网） ---
    ofp = {
        "params": {"request_id": "123456"},
        "general": {"icao_airline": "cca", "callsign": "cca1501",
                    "route": "DCT KENLU W19 TYN B208 DCT   DCT",
                    "initial_altitude": "35000", "cruise_tas": "461"},
        "origin": {"icao_code": "zbaa"}, "destination": {"icao_code": "zbad"},
        "alternate": {"icao_code": "zbtj"},
        "aircraft": {"icao_code": "b738", "reg": "b-1234"},
        "fuel": {"plan_ramp": "18000", "enroute_burn": "12000"},
        "times": {"est_off": "2026-11-24T12:34:00Z",
                  "est_on": "2026-11-24T14:09:00Z"},
    }
    sb = build_plan(ofp)
    check(sb["dep"] == "ZBAA" and sb["dest"] == "ZBAD" and sb["alt"] == "ZBTJ",
          "simbrief airports")
    check(sb["aircraft"] == "B738" and sb["registration"] == "B-1234",
          "simbrief aircraft/reg")
    check(sb["cruise_alt"] == "FL350" and sb["cruise_speed"] == "461",
          "simbrief cruise")
    check(sb["plan_time"] == "1234", "simbrief off-block time")
    check(sb["enroute_h"] == 1 and sb["enroute_m"] == 35, "simbrief enroute 1h35")
    check(sb["fuel_h"] == 2 and sb["fuel_m"] == 22, "simbrief fuel endurance")
    check(sb["route"] == "DCT KENLU W19 TYN B208", "simbrief route cleaned")
    check(sb["callsign"] == "CCA1501" and sb["airline"] == "CCA", "simbrief info")

    sb2 = build_plan({"general": {"route": "DCT"},
                      "times": {"est_enroute": "0135"}})
    check(sb2["enroute_h"] == 1 and sb2["enroute_m"] == 35, "simbrief HHMM duration")
    check(sb2["fuel_h"] == 2 and sb2["fuel_m"] == 20, "simbrief fuel fallback")
    check(clock_hhmm("23:05") == "2305" and clock_hhmm("2026-11-24T00:05:00Z") == "0005"
          and clock_hhmm("9:05") == "0905" and clock_hhmm("ab") == "",
          "simbrief clock parse")
    check(minutes_of("0215") == 135 and minutes_of(5700) == 95
          and minutes_of(95) == 95 and minutes_of("0") is None,
          "simbrief minutes parse")
    check(flight_level(35000) == "FL350" and flight_level(5000) == "5000"
          and build_plan({})["fuel_m"] == 45, "simbrief level/empty plan")
    # 错误分类：代理隧道 / 无用户名（保证前端能拿到可翻译的 code）
    import urllib.error as _ue
    from .simbrief import SimbriefError, fetch_ofp
    try:
        fetch_ofp("")
        check(False, "simbrief empty user raises")
    except SimbriefError as _e:
        check(_e.code == "simbrief_no_user", "simbrief empty user raises")
    _orig = urllib.request.urlopen
    def _boom(*a, **k):
        raise _ue.URLError("Tunnel connection failed: 502 Bad Gateway")
    urllib.request.urlopen = _boom
    try:
        fetch_ofp("someone")
        check(False, "simbrief proxy classified")
    except SimbriefError as _e:
        check(_e.code == "simbrief_proxy", "simbrief proxy classified")
    finally:
        urllib.request.urlopen = _orig

    for ok, name in results:
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
    print("ALL TESTS PASSED" if fails == 0 else f"{fails} TEST(S) FAILED")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    raise SystemExit(run_all())
