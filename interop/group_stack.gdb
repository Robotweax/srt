set pagination off
set confirm off
set print frame-arguments none
set print entry-values no
set may-call-functions off
echo GROUP_STACK_BEGIN\n
thread apply all bt 24
python
import gdb
fields = ("m_bSynRecving", "m_iRcvTimeOut", "m_bClosing", "m_bConnected", "m_RcvBaseSeqNo", "m_RcvEID")
for thread in gdb.selected_inferior().threads():
    thread.switch()
    frame = gdb.newest_frame()
    while frame:
        name = frame.name() or ""
        if "CUDTGroup::recv" in name:
            gdb.write("GROUP_STATE thread=%s function=%s\n" % (thread.num, name))
            try:
                value = frame.read_var("this").dereference()
                for field in fields:
                    try:
                        gdb.write("%s=%d\n" % (field, int(value[field])))
                    except (gdb.error, ValueError):
                        gdb.write("%s=unavailable\n" % field)
            except gdb.error:
                gdb.write("this=unavailable\n")
        frame = frame.older()
end
echo GROUP_STACK_END\n
detach
quit
