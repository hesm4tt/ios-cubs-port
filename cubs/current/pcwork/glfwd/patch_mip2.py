#!/usr/bin/env python3
"""patch_mip2.py GLSRV.C: from "same small mipmap -> skip" to "rows whose blocks changed -> read those rows (plus a block of margin)"."""
import sys

path = sys.argv[1]
s = open(path).read()
old = '''			if (have_mip && smip[key] && smip_size[key] == msz && memcmp(smip[key], mipscratch, msz) == 0) {
				r0 = r1 = 0;                    /* unchanged: nothing is read, the client's copy stands */
				op_count[113]++;
			}'''
assert old in s
new = '''			if (have_mip && smip[key] && smip_size[key] == msz) {
				uint32_t mw = a[2] >> 3 ? a[2] >> 3 : 1, mh = a[3] >> 3 ? a[3] >> 3 : 1;
				size_t rowb = (size_t)mw * 4;
				long first = -1, last = -1;

				for (uint32_t my = 0; my < mh; my++) {
					if (memcmp(smip[key] + my * rowb, mipscratch + my * rowb, rowb) != 0) {
						if (first < 0) {
							first = my;
						}
						last = my;
					}
				}
				if (first < 0) {
					r0 = r1 = 0;                    /* unchanged: nothing is read, the client's copy stands */
					op_count[113]++;
				} else {
					long y0 = first * 8 - 8, y1 = (last + 1) * 8 + 8;

					if (y0 < 0) {
						y0 = 0;
					}
					if (y1 > (long)a[3]) {
						y1 = (long)a[3];
					}
					r0 = (size_t)y0;
					r1 = (size_t)y1;
					op_count[114]++;
				}
			}'''
s = s.replace(old, new, 1)
s = s.replace('(i >= 105 && i <= 113 && op_count[i])', '(i >= 105 && i <= 114 && op_count[i])', 1)
open(path, 'w', newline='\n').write(s)
print('patched')
