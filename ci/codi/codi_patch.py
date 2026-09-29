"""Hook cosmo_usb into codi-app: python3 codi_patch.py NAME SRC DST (SRC is the pristine *.orig)."""
import sys

PATCHES = {
    # server mode only: the one-shot command paths exit before this line
    "codiServer.py": ("EventListener.init()\n",
                      "EventListener.init()\nimport cosmo_usb\ncosmo_usb.start()\n"),
    "codi_st32_generated_functions.py": (
        "    sessionId, msg = readUint32(msg)\n",
        "    sessionId, msg = readUint32(msg)\n"
        "    if cmdId == CMD_SYNC_USB_STATUS:\n"
        "        import cosmo_usb\n"
        "        cosmo_usb.on_usb_status(msg)\n"
        "        return\n"),
}


def patch(name, text):
    anchor, repl = PATCHES[name]
    if text.count(anchor) != 1:
        raise ValueError("%s: anchor %r not found exactly once" % (name, anchor.strip()))
    return text.replace(anchor, repl)


if __name__ == "__main__":
    name, src, dst = sys.argv[1:4]
    with open(src) as f:
        out = patch(name, f.read())
    with open(dst, "w") as f:
        f.write(out)
