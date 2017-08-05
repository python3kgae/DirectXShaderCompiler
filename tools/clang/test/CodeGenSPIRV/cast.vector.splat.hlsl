// Run: %dxc -T vs_6_0 -E main

void main() {
// CHECK-LABEL: %bb_entry = OpLabel

    // From constant
// CHECK:      [[v4f32c:%\d+]] = OpCompositeConstruct %v4float %float_1 %float_1 %float_1 %float_1
// CHECK-NEXT: OpStore %vf4 [[v4f32c]]
    float4 vf4 = 1;
// CHECK-NEXT: [[v3f32c:%\d+]] = OpCompositeConstruct %v3float %float_2 %float_2 %float_2
// CHECK-NEXT: OpStore %vf3 [[v3f32c]]
    float3 vf3;
    vf3 = float1(2);

// CHECK-NEXT: [[si:%\d+]] = OpLoad %int %si
// CHECK-NEXT: [[vi4:%\d+]] = OpCompositeConstruct %v4int [[si]] [[si]] [[si]] [[si]]
// CHECK-NEXT: OpStore %vi4 [[vi4]]
    int si;
    int4 vi4 = si;
// CHECK-NEXT: [[si1:%\d+]] = OpLoad %int %si1
// CHECK-NEXT: [[vi3:%\d+]] = OpCompositeConstruct %v3int [[si1]] [[si1]] [[si1]]
// CHECK-NEXT: OpStore %vi3 [[vi3]]
    int1 si1;
    int3 vi3;
    vi3 = si1;
}
