import io
import sys

path = r"C:\re6vr\src\openxr_bridge.cpp"
with io.open(path, "r", encoding="utf-8") as f:
    lines = f.readlines()

# locate the block to cut: from the compute_eye_matrix comment through the end
# of the Impl struct (the standalone member declarations now live up top).
start = None
for i, ln in enumerate(lines):
    if "Fills `view_proj` (row-major, D3D convention)" in ln:
        start = i
        break
if start is None:
    sys.exit("start marker not found")

end = None
for j in range(start, len(lines)):
    if lines[j].rstrip("\n") == "};":
        end = j
        break
if end is None:
    sys.exit("end marker not found")

new_block = r'''    // Draws the game frame onto a world-locked panel for one eye. The runtime's
    // own per-eye FOV is used so the panel is seen without keystone distortion
    // from any head position.
    bool draw_quad(IDirect3DDevice9 *d3d9_dev, uint32_t image_index,
                   const XrPosef &eye_pose, const XrFovf &eye_fov) {
        // 1) mirror the game's back buffer into the D3D9Ex shared surface
        if (!shared_tex) return false;
        IDirect3DSurface9 *bb = nullptr;
        if (FAILED(d3d9_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, (void **)&bb)) || !bb) {
            warn_once("GetBackBuffer");
            return false;
        }
        IDirect3DSurface9 *dst = nullptr;
        if (SUCCEEDED(shared_tex->GetSurfaceLevel(0, &dst)) && dst) {
            // Same size/format -> StretchRect is a plain GPU blit.
            HRESULT hr = d3d9_dev->StretchRect(bb, nullptr, dst, nullptr, D3DTEXF_NONE);
            if (FAILED(hr)) warn_once_hr("StretchRect(backbuffer -> shared)", hr);
            dst->Release();
        } else {
            warn_once("GetSurfaceLevel(shared)");
        }
        bb->Release();

        // 2) draw the panel into this eye's swapchain image
        ID3D11Texture2D *target = sc_images[image_index].texture;
        ID3D11RenderTargetView *rtv = nullptr;
        if (FAILED(d11_device->CreateRenderTargetView(target, nullptr, &rtv)) || !rtv) {
            warn_once("CreateRenderTargetView(swapchain image)");
            return false;
        }

        // Panel sized so that it exactly fills the eye's field of view.
        const float dist = 2.5f;
        const float tan_l = tanf(eye_fov.angleLeft);
        const float tan_r = tanf(eye_fov.angleRight);
        const float tan_u = tanf(eye_fov.angleUp);
        const float tan_d = tanf(eye_fov.angleDown);
        const float panel_w = dist * (tan_r - tan_l);
        const float panel_h = dist * (tan_u - tan_d);
        // Centre offset in the eye's own basis, then mapped through the eye
        // orientation so a rotated head still sees a level panel.
        const float off_x = dist * 0.5f * (tan_r + tan_l);
        const float off_y = dist * 0.5f * (tan_u + tan_d);

        const float q[4] = {eye_pose.orientation.x, eye_pose.orientation.y,
                            eye_pose.orientation.z, eye_pose.orientation.w};
        const float right[3] = {1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2]),
                                2.0f * (q[0] * q[1] + q[3] * q[2]),
                                2.0f * (q[0] * q[2] - q[3] * q[1])};
        const float up[3] = {2.0f * (q[0] * q[1] - q[3] * q[2]),
                             1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]),
                             2.0f * (q[1] * q[2] + q[3] * q[0])};
        const float fwd[3] = {-2.0f * (q[0] * q[2] + q[3] * q[1]),
                              -2.0f * (q[1] * q[2] - q[3] * q[0]),
                              -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]))};

        const XrVector3f eye = eye_pose.position;
        XrVector3f center;
        center.x = eye.x + fwd[0] * dist + right[0] * off_x + up[0] * off_y;
        center.y = eye.y + fwd[1] * dist + right[1] * off_x + up[1] * off_y;
        center.z = eye.z + fwd[2] * dist + right[2] * off_x + up[2] * off_y;

        // View matrix: bring the panel into the eye's basis, panel plane at z = -dist.
        float view[16];
        view[0] = right[0]; view[4] = right[1]; view[8]  = right[2];
        view[1] = up[0];    view[5] = up[1];    view[9]  = up[2];
        view[2] = -fwd[0];  view[6] = -fwd[1];  view[10] = -fwd[2];
        const float dx = center.x - eye.x, dy = center.y - eye.y, dz = center.z - eye.z;
        view[12] = -(right[0] * dx + right[1] * dy + right[2] * dz);
        view[13] = -(up[0] * dx + up[1] * dy + up[2] * dz);
        view[14] = (fwd[0] * dx + fwd[1] * dy + fwd[2] * dz);
        view[3] = view[7] = view[11] = 0.0f; view[15] = 1.0f;

        // D3D-style asymmetric projection for the panel's field of view.
        const float near_z = 0.05f, far_z = 50.0f;
        float proj[16] = {0};
        proj[0] = 2.0f / (tan_r - tan_l);
        proj[5] = 2.0f / (tan_u - tan_d);
        proj[8] = (tan_r + tan_l) / (tan_r - tan_l);
        proj[9] = (tan_u + tan_d) / (tan_u - tan_d);
        proj[10] = far_z / (far_z - near_z);
        proj[11] = 1.0f;
        proj[14] = -(far_z * near_z) / (far_z - near_z);

        struct {
            float view_proj[16];
            float scale_bias[4];
        } cb;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                float acc = 0.0f;
                for (int k = 0; k < 4; ++k) acc += view[r * 4 + k] * proj[k * 4 + c];
                cb.view_proj[r * 4 + c] = acc;
            }
        }
        cb.scale_bias[0] = panel_w * 0.5f;
        cb.scale_bias[1] = panel_h * 0.5f;
        cb.scale_bias[2] = 0.0f;
        cb.scale_bias[3] = 0.0f;

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(d11_context->Map(quad_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, &cb, sizeof(cb));
            d11_context->Unmap(quad_cb, 0);
        }

        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        d11_context->ClearRenderTargetView(rtv, clear);
        d11_context->OMSetRenderTargets(1, &rtv, nullptr);

        D3D11_VIEWPORT vpd = {};
        vpd.Width = (float)sc_width;
        vpd.Height = (float)sc_height;
        vpd.MinDepth = 0.0f;
        vpd.MaxDepth = 1.0f;
        d11_context->RSSetViewports(1, &vpd);

        UINT stride = sizeof(float) * 5, offset = 0;
        d11_context->IASetInputLayout(quad_layout);
        d11_context->IASetVertexBuffers(0, 1, &quad_vb, &stride, &offset);
        d11_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        d11_context->VSSetShader(quad_vs, nullptr, 0);
        d11_context->VSSetConstantBuffers(0, 1, &quad_cb);
        d11_context->PSSetShader(quad_ps, nullptr, 0);
        d11_context->PSSetShaderResources(0, 1, &shared_srv);
        d11_context->PSSetSamplers(0, 1, &quad_sampler);
        d11_context->RSSetState(quad_raster);
        d11_context->Draw(4, 0);

        ID3D11RenderTargetView *null_rtv[1] = {nullptr};
        d11_context->OMSetRenderTargets(1, null_rtv, nullptr);
        ID3D11ShaderResourceView *null_srv[1] = {nullptr};
        d11_context->PSSetShaderResources(0, 1, null_srv);
        rtv->Release();
        return true;
    }

    bool create_frame_srv() {
        if (shared_srv) return true;
        if (!shared_d11_tex) return false;
        if (FAILED(d11_device->CreateShaderResourceView(shared_d11_tex, nullptr, &shared_srv))) {
            warn_once("CreateShaderResourceView(shared)");
            return false;
        }
        return true;
    }

    void destroy_frame_srv() {
        if (shared_srv) { shared_srv->Release(); shared_srv = nullptr; }
    }

    // Rate-limited diagnostics: some of these can fire once per frame.
    void warn_once(const char *what) {
        for (auto &s : warned) {
            if (s == what) return;
        }
        warned.push_back(what);
        VRLOG("openxr: WARN %s failed (further occurrences suppressed)", what);
    }
    void warn_once_hr(const char *what, HRESULT hr) {
        for (auto &s : warned) {
            if (s == what) return;
        }
        warned.push_back(what);
        VRLOG("openxr: WARN %s failed: 0x%08lX (further occurrences suppressed)", what, (unsigned long)hr);
    }
    std::vector<const char *> warned;
};
'''

out = lines[:start] + [new_block] + lines[end + 1:]
with io.open(path, "w", encoding="utf-8", newline="\n") as f:
    f.writelines(out)
print("replaced lines %d..%d with %d new lines" % (start + 1, end + 1, new_block.count("\n")))
