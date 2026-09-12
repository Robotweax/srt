"""Outer operator: forward interruptions to the owning runner; restore UDP independently."""
from pathlib import Path
import argparse,json,subprocess,signal,time,os
KEYS=['net.core.rmem_max','net.core.wmem_max']
SECURITY=['kernel.perf_event_paranoid','kernel.kptr_restrict']
def main():
 p=argparse.ArgumentParser();p.add_argument('--output-directory',type=Path,required=True);p.add_argument('command',nargs=argparse.REMAINDER);args=p.parse_args()
 command=args.command[1:] if args.command[:1]==['--'] else args.command
 assert command
 root=args.output_directory;root.mkdir(exist_ok=False)
 child=None;pending=None;status=1
 e={'started_ns':time.time_ns(),'command':command,'runner_exit_code':None,'interruption_signal':None,'restoration':[],'uid':os.getuid()}
 def save():
  tmp=root/'execution.json.tmp';tmp.write_text(json.dumps(e,indent=2)+'\n');tmp.replace(root/'execution.json')
 def values(keys):return {k:subprocess.check_output(['sysctl','-n',k],text=True).strip() for k in keys}
 original=values(KEYS);e['original_sysctls']=original;e['security_before']=values(SECURITY);save()
 watched=[signal.SIGINT,signal.SIGTERM,signal.SIGHUP]
 def interrupted(sig,frame):
  nonlocal pending
  if pending is not None:return
  pending=sig;e['interruption_signal']=sig
  # The pinned runner owns the case session; it cleans/reaps that group.
  if child is not None:
   try:child.send_signal(sig)
   except ProcessLookupError:pass
 for sig in watched:signal.signal(sig,interrupted)
 try:
  for k in KEYS:
   if pending is not None:break
   subprocess.run(['sudo','-n','sysctl','-w',k+'=33554432'],check=True,capture_output=True,text=True)
  e['measurement_sysctls']=values(KEYS)
  if pending is None:
   e['processes_before']=subprocess.check_output(['ps','-eo','pid,comm,pcpu,args','--sort=-pcpu'],text=True);save()
   # Block delivery across Popen and assignment, then deliver pending signals.
   previous=signal.pthread_sigmask(signal.SIG_BLOCK,watched)
   try:
    with (root/'runner.stdout').open('x') as out,(root/'runner.stderr').open('x') as err:
     child=subprocess.Popen(command,stdout=out,stderr=err,start_new_session=True,preexec_fn=lambda:signal.pthread_sigmask(signal.SIG_SETMASK,previous))
     signal.pthread_sigmask(signal.SIG_SETMASK,previous)
     if pending is not None:
      try:child.send_signal(pending)
      except ProcessLookupError:pass
     e['runner_pid']=child.pid
     status=child.wait();e['runner_exit_code']=status
   finally:signal.pthread_sigmask(signal.SIG_SETMASK,previous)
  else:status=128+pending
 except BaseException as error:
  e['operator_error']=repr(error)
  if child is not None and child.poll() is None:
   child.send_signal(signal.SIGINT);status=child.wait();e['runner_exit_code']=status
  else:status=1
 finally:
  for sig in watched:signal.signal(sig,signal.SIG_IGN)
  for k,v in original.items():
   try:
    r=subprocess.run(['sudo','-n','sysctl','-w',k+'='+v],capture_output=True,text=True)
    e['restoration'].append({'key':k,'exit_code':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
   except BaseException as error:e['restoration'].append({'key':k,'error':repr(error)})
  try:
   e['restored_sysctls']=values(KEYS);e['security_after']=values(SECURITY)
   e['cleanup_pass']=e['restored_sysctls']==original and e['security_after']==e['security_before'] and all(x.get('exit_code')==0 for x in e['restoration'])
  except BaseException as error:e['cleanup_pass']=False;e['cleanup_error']=repr(error)
  # Preserve a nonzero original runner result even if cleanup also fails.
  if status==0 and not e['cleanup_pass']:status=1
  e['operator_exit_code']=status;e['finished_ns']=time.time_ns();save()
 return status
if __name__=='__main__':raise SystemExit(main())
