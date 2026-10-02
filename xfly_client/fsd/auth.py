"""VATSIM 风格 challenge/response 认证（xPilot client id，非官方）。

忠实移植 FsdAuthState.cs 的状态机：
- init(client_id, private_key)：设置客户端 ID 与初始密钥
- round_received(server_challenge)：计算 $ID 中使用的客户端挑战密钥
- respond_to_challenge(challenge)：回应 $ZC 挑战并推进状态
"""

from __future__ import annotations

import hashlib


class FsdAuthState:
    def __init__(self) -> None:
        self._client_id = 0
        self._init_state = ""
        self._curr_state = ""
        self.client_challenge_key = ""

    def init(self, client_id: int, private_key: str) -> None:
        self._client_id = client_id
        self._curr_state = private_key

    def round_received(self, server_challenge: str) -> None:
        self.client_challenge_key = self._obfuscation_scheme(server_challenge)
        self._init_state = self.client_challenge_key
        self._curr_state = self.client_challenge_key

    def respond_to_challenge(self, challenge: str) -> str:
        resp = self._obfuscation_scheme(challenge)
        combined = self._init_state + resp
        self._curr_state = hashlib.md5(
            combined.encode("ascii")).hexdigest()
        return resp

    def _obfuscation_scheme(self, challenge: str) -> str:
        half = len(challenge) // 2
        c1 = challenge[:half]
        c2 = challenge[half:]
        if self._client_id & 1:
            c1, c2 = c2, c1

        s1 = self._curr_state[0:12]
        s2 = self._curr_state[12:22]
        s3 = self._curr_state[22:32]

        m = self._client_id % 3
        if m == 0:
            h = s1 + c1 + s2 + c2 + s3
        elif m == 1:
            h = s2 + c1 + s3 + c2 + s1
        else:
            h = s3 + c1 + s1 + c2 + s2

        return hashlib.md5(h.encode("ascii")).hexdigest()
