from __future__ import annotations
import copy,json,signal,sys,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'benchmarks'))
import run_pacer_continuation_ab as runner
import test_bounded_send_ab as fixtures
import test_bounded_send_operator as operators

class PacerContinuationOperatorTests(operators.BoundedSendOperatorTests):
    runner_module='run_pacer_continuation_ab'
    modes=('success','runner-failure','SIGINT','SIGTERM','SIGHUP','restore-first-fails')

class PacerContinuationABTests(unittest.TestCase):
    def raw(self,variant='baseline',gain=True):
        raw=fixtures.BoundedSendABTests().fixture(runner.STAGE,variant,improvement=gain)
        for case in raw['runs']:
            result=case['result'];result['peer_exit_codes']={'sender':0,'receiver':0}
            case['peer_exit_status']={'peers':{side:{'returncode':0,'pid':result['peer_process_resources'][role]['pid']} for role,side in [('sender','caller'),('receiver','listener')]}}
        return raw
    def report(self,gain=True):
        return {'complete':True,'interrupted':False,'blocks':[{'variant':v,'analysis':runner.analyze_block(self.raw(v,gain),{},0)} for v in runner.plan()]}
    def test_fixed_24_cases_and_known_gain(self):
        report=self.report();a=runner.analyze_experiment(report)
        self.assertEqual(runner.plan(),['baseline','candidate','candidate','baseline'])
        self.assertEqual(sum(len(b['analysis']['cases']) for b in report['blocks']),24)
        self.assertEqual(a['followup_candidates'],['candidate']);self.assertTrue(a['comparison_valid'])
        self.assertFalse(a['long_transfer_qualification_performed'])
        for p in a['profiles']:self.assertAlmostEqual(p['candidate_over_baseline']['mbps'],1.2)
    def test_raw_wait_status_and_pid_are_required(self):
        for mutate in (lambda c:c.pop('peer_exit_status'),lambda c:c['result'].pop('peer_exit_codes'),lambda c:c['peer_exit_status']['peers']['caller'].update(returncode=7),lambda c:c['peer_exit_status']['peers']['listener'].update(pid=-1),
                       lambda c:c.update(peer_exit_status=None),lambda c:c.update(peer_exit_status=[]),
                       lambda c:c['peer_exit_status'].update(peers=None),lambda c:c['peer_exit_status'].update(peers=[]),
                       lambda c:c['peer_exit_status']['peers'].update(caller=None),lambda c:c['peer_exit_status']['peers'].update(listener=[]),
                       lambda c:c['result']['peer_process_resources'].update(sender=None),
                       lambda c:c['result']['peer_exit_codes'].update(sender=False),
                       lambda c:c['peer_exit_status']['peers']['caller'].update(returncode=False)):
            raw=self.raw();mutate(raw['runs'][0]);self.assertFalse(runner.analyze_block(raw,{},0)['measurement_contract_pass'])
    def test_both_profiles_check_rate_and_sender_total_cpu(self):
        for metric,value in [('mbps',1),('sender_cpu_seconds_per_gib',100),('total_cpu_seconds_per_gib',100)]:
            r=self.report()
            for b in r['blocks']:
                if b['variant']=='candidate':
                    for row in b['analysis']['cases']:
                        if row['profile']==runner.PROFILES[1]:row[metric]=value
            a=runner.analyze_experiment(r);self.assertTrue(a['comparison_valid']);self.assertFalse(a['candidate_gate']['eligible_for_long_transfer_followup'])
    def test_invalid_cases_and_controls_always_name_candidate_reason(self):
        for mutate in (lambda r:r.update(complete=False),lambda r:r['blocks'].insert(1,r['blocks'].pop(0)),lambda r:r['blocks'][0]['analysis'].update(measurement_contract_pass=False),lambda r:r['blocks'][0]['analysis']['cases'][0].update(mbps=9999)):
            r=self.report();mutate(r);a=runner.analyze_experiment(r)
            self.assertFalse(a['comparison_valid']);self.assertTrue(a['candidate_gate']['reasons']);self.assertEqual(a['followup_candidates'],[])
    def execute(self,out,mode=None):
        calls=[]
        def diagnostic(argv):
            dest=Path(argv[argv.index('--output-directory')+1]);dest.mkdir()
            variant='candidate' if 'candidate' in dest.name else 'baseline';calls.append(dest)
            if mode=='interrupt' and len(calls)==2:raise KeyboardInterrupt
            raw=self.raw(variant,gain=mode!='no-gain')
            if mode=='null-status' and len(calls)==1:raw['runs'][0]['peer_exit_status']=None
            (dest/'report.json').write_text('bad-json' if mode=='json' and len(calls)==1 else json.dumps(raw))
            return 7 if mode=='exit' and len(calls)==1 else 0
        report={}
        with patch.object(runner.diag,'main',side_effect=diagnostic):
            code=runner.run_blocks(dict.fromkeys(runner.REVISIONS,Path('manifest')),dict.fromkeys(runner.REVISIONS,{}),out,report)
        return code,report,calls
    def test_valid_comparison_can_end_without_candidate_gain(self):
        for mode in (None,'no-gain'):
            with tempfile.TemporaryDirectory() as d:code,r,calls=self.execute(Path(d)/'out',mode)
            self.assertEqual(code,0);self.assertEqual(len(calls),4);self.assertTrue(r['analysis']['comparison_valid'])
            self.assertEqual(r['analysis']['followup_candidates'],[] if mode else ['candidate'])
    def test_failed_cases_keep_four_attempts_and_original_exit(self):
        for mode in ('exit','json','null-status'):
            with tempfile.TemporaryDirectory() as d:code,r,calls=self.execute(Path(d)/'out',mode)
            self.assertEqual(code,1);self.assertEqual(len(calls),4);self.assertTrue(r['complete'])
            self.assertEqual(r['blocks'][0]['exit_code'],7 if mode=='exit' else 0)
            self.assertTrue(r['analysis']['candidate_gate']['reasons'])
    def test_interrupt_preserved_through_cleanup_error_and_suppression(self):
        previous=signal.getsignal(signal.SIGINT)
        with tempfile.TemporaryDirectory() as d:code,r,calls=self.execute(Path(d)/'out','interrupt')
        self.assertEqual(code,130);self.assertEqual(len(calls),2);self.assertIs(signal.getsignal(signal.SIGINT),previous)
        for suppress in (False,True):
            def diagnostic(_argv):
                try:signal.raise_signal(signal.SIGTERM)
                except KeyboardInterrupt:
                    if suppress:return 7
                    raise PermissionError('injected secondary cleanup failure')
            with tempfile.TemporaryDirectory() as d,patch.object(runner.diag,'main',side_effect=diagnostic):
                r={};code=runner.run_blocks({'baseline':Path('manifest')},{},Path(d)/'out',r)
            self.assertEqual(code,143);self.assertEqual(len(r['blocks']),1)
            self.assertEqual(r['blocks'][0]['driver_exit_code_before_interruption'],7 if suppress else 1)
    def test_manifest_pins_and_overlay_guard(self):
        seed=fixtures.fixtures.PlainCapacityABTests().manifests()['baseline']
        m={v:copy.deepcopy(seed) for v in runner.REVISIONS}
        for v,pin in runner.REVISIONS.items():m[v]['sources']['robotweax']['revision']=pin
        args=({'dirty':False,'revision':'common-harness'},{'platform':'Linux-test','architecture':'aarch64'})
        runner.common.validate_manifests(m,*args,revisions=runner.REVISIONS)
        for mutate in (lambda x:x['candidate']['sources']['robotweax'].update(revision='wrong'),lambda x:x['candidate'].update(poll_counters={'enabled':True})):
            bad=copy.deepcopy(m);mutate(bad)
            with self.assertRaises(ValueError):runner.common.validate_manifests(bad,*args,revisions=runner.REVISIONS)

if __name__=='__main__':unittest.main()
