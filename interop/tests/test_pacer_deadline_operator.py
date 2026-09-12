from pathlib import Path
import json,os,subprocess,signal,tempfile,time,sys
import unittest

@unittest.skipUnless(os.name == "posix" and hasattr(signal, "pthread_sigmask"), "POSIX signal masks and process groups")
class DeadlineOperatorTests(unittest.TestCase):
    def test_exit_signals_and_independent_restoration(self):
        base=Path(__file__).resolve().parents[2]/'benchmarks';harness=base;results=[]
        for mode in ['success','runner-failure','SIGINT','SIGTERM','restore-first-fails']:
         with tempfile.TemporaryDirectory() as d:
          root=Path(d);bin=root/'bin';bin.mkdir();state=root/'state.json';state.write_text(json.dumps({'net.core.rmem_max':'212992','net.core.wmem_max':'212992','kernel.perf_event_paranoid':'4','kernel.kptr_restrict':'1'}))
          (bin/'sudo').write_text('#!/bin/sh\nshift\nexec "$@"\n');(bin/'sudo').chmod(0o755)
          (bin/'sysctl').write_text('#!/usr/bin/env python3\nimport os,json,sys\nfrom pathlib import Path\np=Path(os.environ["TEST_STATE"]);s=json.loads(p.read_text())\nif sys.argv[1]=="-n":print(s[sys.argv[2]])\nelse:\n k,v=sys.argv[2].split("=")\n if os.environ["TEST_MODE"]=="restore-first-fails" and k=="net.core.rmem_max" and v=="212992":sys.exit(7)\n s[k]=v;p.write_text(json.dumps(s))\n');(bin/'sysctl').chmod(0o755)
          (bin/'ps').write_text('#!/bin/sh\nexit 0\n');(bin/'ps').chmod(0o755)
          driver=root/'driver.py'
          if mode in ['SIGINT','SIGTERM']:
           peer=root/'peer.py';peer.write_text('import signal,time,os\nfrom pathlib import Path\nsignal.signal(signal.SIGINT,signal.SIG_IGN)\nsignal.signal(signal.SIGTERM,signal.SIG_IGN)\nPath('+repr(str(root/'peer-ready'))+').write_text(str(os.getpid()))\ntime.sleep(60)\n')
           case=root/'case.py';case.write_text('import signal,subprocess,sys,time,json,os\nfrom pathlib import Path\nsignal.signal(signal.SIGINT,signal.SIG_IGN)\nsignal.signal(signal.SIGTERM,signal.SIG_IGN)\np=subprocess.Popen([sys.executable,'+repr(str(peer))+'])\nwhile not Path('+repr(str(root/'peer-ready'))+').exists():time.sleep(.01)\nPath('+repr(str(root/'ready.tmp'))+').write_text(json.dumps({"case":os.getpid(),"peer":p.pid}))\nPath('+repr(str(root/'ready.tmp'))+').replace('+repr(str(root/'ready.json'))+')\ntime.sleep(60)\n')
           driver.write_text(f'import sys\nfrom pathlib import Path\nsys.path.insert(0,{str(harness)!r})\nimport run_pacer_deadline_diagnostics as ab\nroot=Path({str(root)!r})\ndef diagnostic(args):\n out=root/"case-output";out.mkdir()\n return ab.diag.run_bounded([sys.executable,str(root/"case.py")],out,90)\nab.diag.main=diagnostic\nraise SystemExit(ab.run_blocks({{"baseline-plain":root/"a"}},{{}},root/"result",{{}}))\n')
          else:driver.write_text('raise SystemExit('+('7' if mode in ['runner-failure','restore-first-fails'] else '0')+')\n')
          env=os.environ|{'PATH':str(bin)+':'+os.environ['PATH'],'TEST_STATE':str(state),'TEST_MODE':mode}
          proc=subprocess.Popen([sys.executable,str(base/'run_pacer_deadline_operator.py'),'--output-directory',str(root/'operator'),'--',sys.executable,str(driver)],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
          ids={}
          try:
           if mode in ['SIGINT','SIGTERM']:
            limit=time.monotonic()+8
            while not (root/'ready.json').exists() and time.monotonic()<limit:time.sleep(.01)
            ids=json.loads((root/'ready.json').read_text());proc.send_signal(getattr(signal,mode))
           _,err=proc.communicate(timeout=15);e=json.loads((root/'operator/execution.json').read_text())
           expected=128+getattr(signal,mode) if mode.startswith('SIG') else 7 if mode in ['runner-failure','restore-first-fails'] else 0
           assert proc.returncode==expected and e['runner_exit_code']==expected,(mode,e,err)
           assert e['cleanup_pass']==(mode!='restore-first-fails')
           assert len(e['restoration'])==2 and json.loads(state.read_text())['net.core.wmem_max']=='212992'
           for pid in ids.values():
            observed=subprocess.run(['/bin/ps','-o','stat=','-p',str(pid)],capture_output=True,text=True).stdout.strip();assert not observed or observed.startswith('Z'),observed
           results.append({'mode':mode,'pass':True,'expected_exit_code':expected,'execution':e,'case_and_peer_stopped':True if ids else None})
          finally:
           if proc.poll() is None:proc.send_signal(signal.SIGTERM);proc.wait(timeout=15)
        print(json.dumps({'tests_pass':True,'synthetic_only':True,'cases':results},indent=2))

if __name__ == "__main__":
    unittest.main()
