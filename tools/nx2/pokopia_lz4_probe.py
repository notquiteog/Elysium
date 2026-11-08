"""Minimal LZ4 raw-block decoder, enough to tell whether a buffer is one."""
def decompress(src, start=0, max_out=1 << 31):
    out = bytearray()
    i = start
    n = len(src)
    while i < n:
        token = src[i]; i += 1
        lit = token >> 4
        if lit == 15:
            while i < n:
                b = src[i]; i += 1
                lit += b
                if b != 255:
                    break
        if i + lit > n:
            raise ValueError("literals past end")
        out += src[i:i + lit]; i += lit
        if len(out) > max_out:
            raise ValueError("output too large")
        if i >= n:
            break                       # last sequence is literals only
        if i + 2 > n:
            raise ValueError("truncated offset")
        off = src[i] | (src[i + 1] << 8); i += 2
        if off == 0 or off > len(out):
            raise ValueError("bad match offset")
        ml = token & 0xF
        if ml == 15:
            while i < n:
                b = src[i]; i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        p = len(out) - off
        for _ in range(ml):             # byte-wise: matches may overlap
            out.append(out[p]); p += 1
        if len(out) > max_out:
            raise ValueError("output too large")
    return bytes(out)
