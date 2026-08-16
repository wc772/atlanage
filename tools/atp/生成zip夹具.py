



import os
import zipfile

BASE = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "测试", "夹具"))
os.makedirs(BASE, exist_ok=True)

atp_json = "# 夹具清单\n包名: 夹具包\n版本: 1.0.0\n入口: 入口.at\n依赖: 网络请求@^1.0.0\n"
entry_at = ("// 夹具入口: 重复文本用于 deflate LZ77 压缩验证\n" +
            ("你好，世界！重复行 1234567890 abcdefghijklmnopqrstuvwxyz\n" * 30))
data_txt = "二进制安全: \x00\x01\x02 字节原样\n"  

files = {
    "atp.json": atp_json.encode("utf-8"),
    "入口.at": entry_at.encode("utf-8"),
    "数据.txt": data_txt.encode("utf-8"),
}

for name, comp in (("stored.zip", zipfile.ZIP_STORED), ("deflate.zip", zipfile.ZIP_DEFLATED)):
    path = os.path.join(BASE, name)
    with zipfile.ZipFile(path, "w", compression=comp) as zf:
        for fname, content in files.items():
            zf.writestr(fname, content)
    print("生成", path, os.path.getsize(path), "字节")


with zipfile.ZipFile(os.path.join(BASE, "deflate.zip")) as zf:
    for fname, content in files.items():
        assert zf.read(fname) == content, fname
print("夹具校验通过")
