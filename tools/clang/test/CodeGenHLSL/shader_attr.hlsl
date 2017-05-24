// RUN: %dxc -T lib_6_1 %s | FileCheck %s

// CHECK:attribute 'shader' must have one of these values: cs_6_0,cs_6_1,ps_6_0,ps_6_1,vs_6_0,vs_6_1, hs_6_0,hs_6_1,ds_6_0,ds_6_1,gs_6_0,gs_6_1

[shader("lib_6_1")]
float4 ps_main(float4 a : A) : SV_TARGET
{
  return a;
}
