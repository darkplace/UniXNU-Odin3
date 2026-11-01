#!/usr/bin/env python3
"""Build and run Test/gpu-survey.c, then check every register constant of GpuSurvey.h against the pinned sources (gcc-sm8750.c, gpucc-sm8750.c,
a6xx.xml A8XX variant, qcom_scm.h) so that no offset is typed from memory."""
import hashlib, os, re, subprocess, sys, tempfile
from pathlib import Path
here = Path(__file__).resolve().parent
root = here.parents[6]
project = Path(os.environ.get('IDRAGON_PROJECT', root))
pins = __import__('json').loads((root / 'devices/odin3-pagani/gpu/a830-inputs.json').read_text())['linux']
base = project / 'firmware/ref-linux' / pins['commit']
for name, pin in pins['files'].items():
    data = (base / name).read_bytes()
    assert len(data) == pin['bytes'] and hashlib.sha256(data).hexdigest() == pin['sha256'], name
with tempfile.TemporaryDirectory(dir=root) as tmp:
    exe = Path(tmp) / 'gpu-survey'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g', str(here / 'gpu-survey.c'), '-o', str(exe)], check=True)
    out = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout
    print(out.strip())
h = (here.parent / 'GpuSurvey.h').read_text()
def const(name): return int(re.search(r'#define\s+' + name + r'\s+\(?(?:GS_[A-Z_]+_BASE\s*\+\s*)?(0x[0-9a-fA-F]+)', h).group(1), 16)
gpucc = (base / 'drivers/clk/qcom/gpucc-sm8750.c').read_text()
gcc = (base / 'drivers/clk/qcom/gcc-sm8750.c').read_text()
def branch(src, name):
    m = re.search(r'static struct clk_branch ' + name + r' = \{\s*\.halt_reg = (0x[0-9a-f]+)', src); return int(m.group(1), 16)
assert const('GS_GPUCC_CX_GDSCR') == int(re.search(r'\.gdscr = (0x[0-9a-f]+)', gpucc).group(1), 16)
assert const('GS_GPUCC_AHB') == branch(gpucc, 'gpu_cc_ahb_clk') and const('GS_GPUCC_CX_GMU') == branch(gpucc, 'gpu_cc_cx_gmu_clk')
assert const('GS_GPUCC_HLOS1_SMMU') == branch(gpucc, 'gpu_cc_hlos1_vote_gpu_smmu_clk')
assert const('GS_GPUCC_GMU_RCG_CMD') == int(re.search(r'static struct clk_rcg2 gpu_cc_gmu_clk_src = \{\s*\.cmd_rcgr = (0x[0-9a-f]+)', gpucc).group(1), 16)
always_on = [int(x, 16) for x in re.findall(r'(0x[0-9a-f]+), /\* GPU_CC_', gpucc)]
assert {const('GS_GPUCC_RSCC_XO_AON'), const('GS_GPUCC_CXO_AON'), const('GS_GPUCC_CB')} <= set(always_on), always_on
assert const('GS_GCC_GPU_GEMNOC') == branch(gcc, 'gcc_gpu_gemnoc_gfx_clk') and const('GS_GCC_QMIP_GPU_AHB') == branch(gcc, 'gcc_qmip_gpu_ahb_clk')
assert 'qcom_branch_set_clk_en(regmap, 0x71004);' in gcc and const('GS_GCC_GPU_CFG_AHB') == 0x71004
xml = (base / 'drivers/gpu/drm/msm/registers/adreno/a6xx.xml').read_text()
for name, macro in (('CX_MISC_SW_FUSE_FREQ_LIMIT_STATUS', 'GS_CX_MISC_FREQ_LIMIT_STATUS'), ('CX_MISC_TCM_RET_CNTL', 'GS_CX_MISC_TCM_RET_CNTL'), ('CX_MISC_SLICE_ENABLE_FINAL', 'GS_CX_MISC_SLICE_ENABLE_FINAL')):
    dword = int(re.search(r'<reg32 offset="(0x[0-9a-f]+)" name="' + name + '"', xml).group(1), 16)
    m = re.search(r'#define\s+' + macro + r'\s+\(GS_CX_MISC_BASE \+ (0x[0-9a-f]+) \* 4\)', h)
    assert int(m.group(1), 16) == dword, (name, m.group(1), hex(dword))
scm = (base / 'drivers/firmware/qcom/qcom_scm.h').read_text()
for macro, name in (('GS_SCM_SVC_PIL', 'QCOM_SCM_SVC_PIL'), ('GS_SCM_SVC_INFO', 'QCOM_SCM_SVC_INFO'), ('GS_SCM_SVC_GPU', 'QCOM_SCM_SVC_GPU'),
                    ('GS_SCM_PAS_INIT_IMAGE', 'QCOM_SCM_PIL_PAS_INIT_IMAGE'), ('GS_SCM_PAS_MEM_SETUP', 'QCOM_SCM_PIL_PAS_MEM_SETUP'),
                    ('GS_SCM_PAS_AUTH_AND_RESET', 'QCOM_SCM_PIL_PAS_AUTH_AND_RESET'), ('GS_SCM_PAS_IS_SUPPORTED', 'QCOM_SCM_PIL_PAS_IS_SUPPORTED'),
                    ('GS_SCM_INFO_IS_CALL_AVAIL', 'QCOM_SCM_INFO_IS_CALL_AVAIL'), ('GS_SCM_GPU_INIT_REGS', 'QCOM_SCM_SVC_GPU_INIT_REGS')):
    want = int(re.search(r'#define ' + name + r'\s+(0x[0-9a-fA-F]+)', scm).group(1), 16)
    assert int(re.search(r'#define ' + macro + r'\s+(0x[0-9a-fA-F]+)', h).group(1), 16) == want, macro
assert 'SCM_SMC_FNID(s, c)	((((s) & 0xFF) << 8) | ((c) & 0xFF))' in scm
assert re.search(r'#define GPU_PAS_ID 13', (base / 'drivers/gpu/drm/msm/adreno/a8xx_gpu.c').read_text())
print('PASS: GpuSurvey.h constants match the pinned gcc-sm8750.c, gpucc-sm8750.c, a6xx.xml (A8XX) and qcom_scm.h')
