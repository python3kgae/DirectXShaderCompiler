// RUN: %dxc -E main -T ps_6_6 %s -Od | FileCheck %s

Texture2D tex0 : register(t0);
Texture2D tex1 : register(t42);

const static float2 my_offsets[] = {
  float2(1,2),
  float2(3,4),
  float2(5,6),
  float2(7,8),
};

[RootSignature("DescriptorTable(SRV(t0), SRV(t42)), DescriptorTable(Sampler(s0))")]
float4 main(uint2 uv : TEXCOORD) : SV_Target {
  // CHECK: %[[handle:.+]] = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 1, i32 42
  int x = 0;
  int y = 0;

  float2 val = my_offsets[x+y+1];

  Texture2D tex = tex0;
  if (val.x > 0) {
    tex = tex1;
  }
  // CHECK: %[[annotHandle:.+]] = call %dx.types.Handle @dx.op.annotateHandle(i32 217, %dx.types.Handle %[[handle]], i8 0, i8 2, %dx.types.ResourceProperties { i32 -858993463, i32 -858993463 })
  // CHECK: @dx.op.textureLoad.f32(i32 66, %dx.types.Handle %[[annotHandle]]
  return tex.Load(0);
}

