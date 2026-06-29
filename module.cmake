# wxl-render-modern: the post-process effects run a D3D12 backend on the proxy's shared device. The DLL target
# needs the module's own src/ on the include path (effects in src/effects/** include "gpu/Framework.hpp" etc.
# by module-root path, not relative to themselves) and the d3d12 import library (for D3D12SerializeRootSignature;
# every other D3D12 call goes through the ID3D12Device handed over by the proxy via WxlD3D12Device). The vendor
# shaders are pulled in by relative path from each effect, so they need no extra include directory. dxgi is
# header-only here (DXGI_FORMAT enums), so no extra link is needed; d3dcompiler is already linked by the core.
#
# This file is included by the root CMakeLists with the WarcraftXL target already defined.
target_include_directories(WarcraftXL PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
target_link_libraries(WarcraftXL PRIVATE d3d12)
