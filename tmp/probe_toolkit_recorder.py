"""Read-only source audit: execute actual recorder run() with mock writer/timestamps."""
import ast,json,threading
from pathlib import Path
from collections import deque
from types import SimpleNamespace
import numpy as np
root=Path('D:/01_Project/02_Loewe/08_Tool/Thermal_Image_Toolkit')
source=root/'thermal_toolkit/uvc/recording.py'
tree=ast.parse(source.read_text(encoding='utf-8'))
cls=next(x for x in tree.body if isinstance(x,ast.ClassDef) and x.name=='UvcRecordingThread')
run=next(x for x in cls.body if isinstance(x,ast.FunctionDef) and x.name=='run')
module=ast.Module(body=[run],type_ignores=[]);ast.fix_missing_locations(module)
class Writer:
    def __init__(self,*args):self.frames=[];writers.append(self)
    def isOpened(self):return True
    def write(self,f):self.frames.append(int(f[0,0,0]))
    def release(self):pass
def probe(times):
    global writers
    writers=[]
    env={'cv2':SimpleNamespace(VideoWriter=Writer,VideoWriter_fourcc=lambda *x:0),'UVC_RECORDING_FOURCC':'MJPG'}
    exec(compile(module,str(source),'exec'),env)
    self=SimpleNamespace(_condition=threading.Condition(),_queue=deque((i,None,float(t)) for i,t in enumerate(times)),_stop_requested=True,_stop_frame_time=None,video_path=Path('unused-probe.avi'),fps=30.,left_label='',right_label='',_submitted_frames=len(times),_queue_dropped_frames=0,_compose_frame=lambda i,*args:np.full((2,2,3),i,np.int32),result_summary=None,error_message=None)
    env['run'](self);assert self.error_message is None,self.error_message
    frames=writers[0].frames
    repeats=[i for i in range(1,len(frames)) if frames[i]==frames[i-1]]
    return dict(stats=self.result_summary,repeat_indices=repeats)
regular=probe(np.arange(480)/30)
# Constructed monotonically increasing arrival timestamps: 12 arrivals / 0.4 s.
# A delayed frame followed quickly by the next frame crosses then shares a CFR slot.
pattern=[0,.032,.064,.096,.128,.192,.200,.232,.264,.296,.328,.360]
jitter=probe([i//12*.4+pattern[i%12] for i in range(480)])
assert regular['stats']['duplicate_frames']==regular['stats']['timestamp_dropped_frames']==0
assert jitter['stats']['duplicate_frames']==jitter['stats']['timestamp_dropped_frames']==40
assert jitter['repeat_indices']==list(range(5,474,12))
report=dict(source=str(source),scope='Actual run() extracted from current source; mock encoder. Constructed timestamps demonstrate mechanism, not recovered timestamps of the recording.',regular_30fps=regular,arrival_jitter_with_30fps_average=jitter)
dest=Path('D:/01_Project/02_Loewe/01_Report/20261111/video_100444_analysis/toolkit_recorder_probe.json')
dest.write_text(json.dumps(report,indent=2),encoding='utf-8')
for name,res in [('regular',regular),('jitter',jitter)]:print(name,{k:res['stats'][k] for k in ['submitted_frames','written_frames','duplicate_frames','timestamp_dropped_frames']},'repeat positions',res['repeat_indices'][:8])
