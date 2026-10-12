import lzma, os, re, sys, shutil, subprocess
KV = "6.12.111+deb13-amd64"
MODROOT = f"kroot/usr/lib/modules/{KV}/kernel"
paths = {}
for root, _, files in os.walk(MODROOT):
    for f in files:
        if f.endswith(".ko.xz") or f.endswith(".ko"):
            name = f.split(".ko")[0].replace("-", "_")
            paths[name] = os.path.join(root, f)
def data(name):
    p = paths[name]
    b = open(p, "rb").read()
    return lzma.decompress(b) if p.endswith(".xz") else b
def deps(name):
    m = re.search(rb"depends=([^\x00]*)", data(name))
    return [d.decode().replace("-", "_") for d in m.group(1).split(b",") if d] if m else []
order = []
def visit(n):
    if n in order: return
    for d in deps(n): visit(d)
    order.append(n)
targets = ["ahci", "sd_mod", "sg", "nvme", "raid1", "xhci_pci", "usb_storage", "uas", "crc32c_generic"]
for t in targets:
    if t in paths: visit(t)
    else: print("missing", t)
R = "initramfs"
shutil.rmtree(R, ignore_errors=True)
for d in ["bin", "sbin", "mods", "dev", "proc", "sys", "tmp", "lib", "lib64", "usr/lib/x86_64-linux-gnu", "out", "etc"]:
    os.makedirs(os.path.join(R, d), exist_ok=True)
for n in order:
    open(f"{R}/mods/{n}.ko", "wb").write(data(n))
open(f"{R}/mods/order", "w").write("\n".join(order) + "\n")
shutil.copy("/usr/bin/busybox", f"{R}/bin/busybox")
def copy_with_libs(exe, dest):
    shutil.copy(exe, f"{R}{dest}")
    out = subprocess.run(["ldd", exe], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.search(r"(/\S+)", line.split("=>")[-1])
        if m and os.path.exists(m.group(1)):
            lib = m.group(1)
            os.makedirs(os.path.dirname(f"{R}{lib}"), exist_ok=True)
            shutil.copy(lib, f"{R}{lib}")
copy_with_libs("/usr/sbin/smartctl", "/bin/smartctl")
copy_with_libs("/usr/sbin/mdadm", "/bin/mdadm")
shutil.copy(sys.argv[1], f"{R}/bin/hdmon")
if os.path.exists("helpers/setfcap"): shutil.copy("helpers/setfcap", f"{R}/bin/setfcap")
if os.path.exists("helpers/hdmon_tests"): shutil.copy("helpers/hdmon_tests", f"{R}/bin/hdmon_tests")
shutil.copy("init.sh", f"{R}/init")
os.chmod(f"{R}/init", 0o755)
print("modules:", " ".join(order))
subprocess.run(f"cd {R} && find . | busybox cpio -o -H newc 2>/dev/null | gzip -1 > ../initrd.gz", shell=True, check=True)
print("wrote initrd.gz")
