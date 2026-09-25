import sys,struct,zlib
def write_png(path, data, w, h, stride=None):
    # data: bytes of BGRA rows
    raw=bytearray()
    for y in range(h):
        o=y*w*4
        row=data[o:o+w*4]
        raw.append(0)
        raw.extend(row[i+2] for i in range(0,len(row),4))
        raw.extend(row[i+1] for i in range(0,len(row),4))
        raw.extend(row[i] for i in range(0,len(row),4))
    def chunk(t,d):
        c=struct.pack('>I',len(d))+t+d
        return c+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
    png=b'\x89PNG\r\n\x1a\n'
    png+=chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))
    png+=chunk(b'IDAT',zlib.compress(bytes(raw),6))
    png+=chunk(b'IEND',b'')
    open(path,'wb').write(png)
    print('wrote',path,w,'x',h)
src=sys.argv[1]; dst=sys.argv[2]; w=int(sys.argv[3]); h=int(sys.argv[4])
d=open(src,'rb').read()
write_png(dst,d,w,h)
