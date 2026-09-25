import io

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    t = f.read()

# Drop the old CopyJob, and introduce one uniform guarded job used by both the
# per-frame copy and the one-off probe.
s = t.index("    struct CopyJob {")
e = t.index("    bool do_copy(", s)
t = t[:s] + t[e:]

job = r'''    // Work handed to seh::guard. SEH requires the guarded body in its own
    // function, so each operation is a static entry point here.
    struct SehJob {
        Impl *self;
        IDirect3DDevice9 *dev;
        IDirect3DSurface9 *src;
        IDirect3DSurface9 *dst;
        bool is_readback;      // true: GetRenderTargetData(src -> dst)
        bool ok = false;
        HRESULT hr = E_FAIL;

        static void run(void *ctx) {
            SehJob *j = static_cast<SehJob *>(ctx);
            if (j->is_readback) {
                j->hr = j->self->d3d9_owner->lpVtbl->GetRenderTargetData(j->self->d3d9_owner,
                                                                        j->src, j->dst);
                j->ok = SUCCEEDED(j->hr);
            } else {
                j->ok = j->self->do_copy(j->dev, j->src, j->dst);
            }
        }
    };

'''
anchor = "    bool create_device_surfaces(IDirect3DDevice9 *dev, uint32_t w, uint32_t h, D3DFORMAT fmt) {"
t = t.replace(anchor, job + anchor, 1)

# copy_backbuffer uses it
s = t.index("        CopyJob job = {this, d3d9_dev, src_surface, dst};")
e = t.index("        return job.ok;", s) + len("        return job.ok;")
t = t[:s] + r'''        SehJob job = {this, d3d9_dev, src_surface, dst, false, false, E_FAIL};
        const bool completed = seh::guard(&SehJob::run, &job);''' + t[e:]

# probe uses it
s = t.index("            ReadbackJob job = {this, dev, tiny_src};")
e = t.index("            }", t.index("VRLOG(\"probe: GetRenderTargetData returned", s))
t = t[:s] + r'''            SehJob job = {this, dev, tiny_src, tiny_dst, true, false, E_FAIL};
            const bool completed = seh::guard(&SehJob::run, &job);
            if (!completed) {
                VRLOG("probe: GetRenderTargetData RAISED - readback is unusable on this machine");
            } else {
                VRLOG("probe: GetRenderTargetData returned 0x%08lX - readback %s",
                      (unsigned long)job.hr, SUCCEEDED(job.hr) ? "WORKS" : "refused");
            }
        } else if (false) {
            {
''' + t[e:]

with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(t)
print("unified the guarded jobs")
