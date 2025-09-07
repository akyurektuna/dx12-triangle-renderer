# dx12-triangle-renderer
a basic DirectX 12 renderer that draws a rotating triangle, interactive controls using Dear ImGui.

https://github.com/user-attachments/assets/30b0ac21-3435-40b7-aa6a-e2a2a670e67e

# core components
- vertex buffer: gpu side memory for triangle geometry
- constant buffer: dynamic matrix updates for rotation
- root signature: two parameter layout (cbv and srv) for shader resources
- pipeline state object: rendering pipeline config
- descriptor heaps: resource views for render targets and shader resources

# key dx12 concepts
- command list management
- resource barriers and state transitions
- cpu-gpu synchronization with fence
- descriptor heap management
- root signature parameter binding
- shader compilation and pso creation

# manual dx12 implementation (no d3dx12.h)
- this project intentionally avoids using the d3dx12.h helper library to demonstrate a fundamental understanding
  ## implemented utilities
    - resource barriers: manual resorce barrier creation instead of ``` CD3DX12_RESOURCE_BARRIER::Transition() ```
    - descriptor handle management: manual descriptor offsetting instead of ``` CD3DX12_CPU_DESCRIPTOR_HANDLE ```
    - heap properties initialization: manual heap property setup instead of ``` CD3DX12_HEAP_PROPERTIES ```
