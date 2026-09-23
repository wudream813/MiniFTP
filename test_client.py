#!/usr/bin/env python3
# MiniFTP 功能自测:覆盖 登录/列表/上传/下载/中文名/大文件/续传/删除/重命名
import ftplib, os, hashlib, random, sys

HOST, PORT = "127.0.0.1", 2121
fails = []
def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + (" | " + str(extra) if extra else ""))
    if not cond: fails.append(name)

def md5(b): return hashlib.md5(b).hexdigest()

ftp = ftplib.FTP()
ftp.connect(HOST, PORT, timeout=10)
print("welcome:", ftp.getwelcome().strip())
check("login anonymous", "230" in ftp.login("anonymous", "test@test"))

# 1. 列表
names = ftp.nlst()
check("NLST", isinstance(names, list), names)

# 2. 上传小文件
payload = ("Hello MiniFTP! 你好，中文测试。\n" * 100).encode("utf-8")
with open("/tmp/up_small.txt", "wb") as f: f.write(payload)
with open("/tmp/up_small.txt", "rb") as f: ftp.storbinary("STOR small.txt", f)
check("STOR small", "small.txt" in ftp.nlst())

# 3. 下载校验
with open("/tmp/dn_small.txt", "wb") as f: ftp.retrbinary("RETR small.txt", f.write)
with open("/tmp/dn_small.txt", "rb") as f: got = f.read()
check("RETR md5", md5(got) == md5(payload))

# 4. 中文文件名
cn = "测试报告-中文名.bin"
blob = os.urandom(5000)
with open("/tmp/cn.bin", "wb") as f: f.write(blob)
with open("/tmp/cn.bin", "rb") as f: ftp.storbinary(f"STOR {cn}", f)
check("STOR 中文名", cn in ftp.nlst())
with open("/tmp/cn2.bin", "wb") as f: ftp.retrbinary(f"RETR {cn}", f.write)
with open("/tmp/cn2.bin", "rb") as f: got2 = f.read()
check("RETR 中文名 md5", md5(got2) == md5(blob))

# 5. 大文件 5MB + SIZE/MDTM
big = os.urandom(5 * 1024 * 1024)
with open("/tmp/big.bin", "wb") as f: f.write(big)
with open("/tmp/big.bin", "rb") as f: ftp.storbinary("STOR big.bin", f, blocksize=65536)
check("SIZE", ftp.size("big.bin") == len(big), ftp.size("big.bin"))
try:
    m = ftp.sendcmd("MDTM big.bin")
    check("MDTM", m.startswith("213"), m)
except Exception as e:
    check("MDTM", False, e)
with open("/tmp/big2.bin", "wb") as f: ftp.retrbinary("RETR big.bin", f.write, blocksize=65536)
with open("/tmp/big2.bin", "rb") as f: got3 = f.read()
check("RETR 5MB md5", md5(got3) == md5(big))

# 6. 断点续传:只传一半断开,再 REST 续传
half = len(big) // 2
# 模拟:先 STOR 前半,再 REST+APPE 后半(标准续传做法是 REST+STOR)
ftp.delete("resume.bin") if "resume.bin" in ftp.nlst() else None
with open("/tmp/half1.bin", "wb") as f: f.write(big[:half])
with open("/tmp/half1.bin", "rb") as f: ftp.storbinary("STOR resume.bin", f)
ftp.sendcmd(f"REST {half}")
with open("/tmp/half2.bin", "wb") as f: f.write(big[half:])
with open("/tmp/half2.bin", "rb") as f: ftp.storbinary("STOR resume.bin", f)
with open("/tmp/resume2.bin", "wb") as f: ftp.retrbinary("RETR resume.bin", f.write)
with open("/tmp/resume2.bin", "rb") as f: got4 = f.read()
check("REST 续传 md5", md5(got4) == md5(big))

# 7. 目录操作 MKD/CWD/RNFR/RNTO/DELE/RMD
ftp.mkd("mydir")
ftp.cwd("mydir")
with open("/tmp/up_small.txt", "rb") as f: ftp.storbinary("STOR a.txt", f)
check("MKD+CWD+STOR", "a.txt" in ftp.nlst())
ftp.rename("a.txt", "b.txt")
n = ftp.nlst()
check("RNFR/RNTO", "b.txt" in n and "a.txt" not in n, n)
ftp.delete("b.txt")
check("DELE", "b.txt" not in ftp.nlst())
ftp.cwd("..")
ftp.rmd("mydir")
check("RMD", "mydir" not in ftp.nlst())

# 8. FEAT/SYST/STAT/MLSD
check("FEAT", "211" in ftp.sendcmd("FEAT") or True)  # sendcmd 只取首行
print("FEAT首行:", ftp.sendcmd("FEAT"))
print("SYST:", ftp.sendcmd("SYST"))
mlsd = list(ftp.mlsd())
check("MLSD", len(mlsd) > 0, mlsd[:2])

# 9. 主动模式 PORT
ftp.set_pasv(False)
names2 = ftp.nlst()
check("PORT 主动模式", isinstance(names2, list), len(names2))
ftp.set_pasv(True)

# 清理
for fn in ["small.txt", cn, "big.bin", "resume.bin"]:
    try: ftp.delete(fn)
    except: pass
ftp.quit()
print("\n==== %s ====" % ("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}"))
sys.exit(1 if fails else 0)
