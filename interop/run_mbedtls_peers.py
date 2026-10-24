#!/usr/bin/env python3
"""Public C API, bidirectional encrypted rotation between isolated backend builds."""
import argparse
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess

from interop_common import reserved_udp_ports

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--mbedtls-peer', type=Path, required=True)
    parser.add_argument('--openssl-peer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--acp1', action='store_true', help='Require experimental ACP1 on both peers')
    parser.add_argument('--rendezvous', action='store_true', help='Qualify ACP1 simultaneous-open peers')
    parser.add_argument('--group', choices=['broadcast','backup'], help='Qualify ACP1 group joins and replacements')
    args=parser.parse_args()
    if args.rendezvous and not args.acp1:
        parser.error('--rendezvous requires --acp1')
    if args.group and not args.acp1:
        parser.error('--group requires --acp1')
    environment=os.environ.copy()
    environment['SRT_INTEROP_PASSPHRASE']=secrets.token_hex(16)
    results=[]
    for mode in ['ctr','gcm']:
        for auth in ([True] if args.acp1 else [False,True]):
            for key in [16,24,32]:
                for caller,listener in [(args.mbedtls_peer,args.openssl_peer),(args.openssl_peer,args.mbedtls_peer)]:
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                        probe.bind(('127.0.0.1',0))
                        port=probe.getsockname()[1]
                    print(f'{mode} auth={auth} key={key} caller={caller.parent.name}',flush=True)
                    common=[str(port),str(int(auth)),str(key),mode]
                    if args.acp1:
                        common.append('1')
                    server_args=['listener',*common]
                    client_args=['caller',*common]
                    if args.group:
                        server_args=[args.group+'-listener',*common]
                        client_args=[args.group+'-caller',*common]
                    if args.rendezvous:
                        # Keep the first path reserved while choosing replacement
                        # endpoints: a later ephemeral allocation may reuse a
                        # released first-path port, including the opposite peer's.
                        with reserved_udp_ports(4 if args.group else 2) as ports:
                            left_port,right_port=ports[:2]
                            if args.group:
                                second_left,second_right=ports[2:]
                        prefix='rdv-'+(args.group+'-' if args.group else '')
                        server_args=[prefix+'listener',str(right_port),*common[1:],str(left_port)]
                        client_args=[prefix+'caller',str(left_port),*common[1:],str(right_port)]
                        if args.group:
                            server_args.extend([str(second_right),str(second_left)])
                            client_args.extend([str(second_left),str(second_right)])
                    server=subprocess.Popen([str(listener.resolve()),*server_args],env=environment,
                        stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                    try:
                        ready=server.stdout.readline()
                        if ready.strip()!='ready':
                            raise RuntimeError('listener startup failed: '+server.stderr.read())
                        client=subprocess.run([str(caller.resolve()),*client_args],env=environment,
                            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=40)
                        output,error=server.communicate(timeout=40)
                        if client.returncode or server.returncode:
                            raise RuntimeError(f'backend transfer failed: caller_exit={client.returncode} listener_exit={server.returncode} caller_args={client_args} listener_args={server_args}\n{client.stdout}\n{client.stderr}\n{output}\n{error}')
                        client_result=json.loads(client.stdout.strip().splitlines()[-1])
                        server_result=json.loads(output.strip().splitlines()[-1])
                        if args.acp1:
                            assert client_result['acp1'] and server_result['acp1']
                        if args.rendezvous:
                            assert client_result['rendezvous'] and server_result['rendezvous']
                        if args.group:
                            assert client_result['group']==server_result['group']==args.group
                        assert client_result['integrity'] and server_result['integrity']
                        assert {client_result['backend'],server_result['backend']}=={1,3}
                        results.append(dict(mode=mode,session_auth=auth,acp1=args.acp1,rendezvous=args.rendezvous,group=args.group,key_bytes=key,
                            caller_backend=client_result['backend'],listener_backend=server_result['backend'],
                            messages_each_direction=512,rotation_packets=32,integrity=True))
                    finally:
                        if server.poll() is None:
                            server.kill()
                            server.communicate()
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(results,indent=2)+'\n')
    print(f'{len(results)} bidirectional CTR/GCM, rotation and session-auth scenarios passed')

if __name__=='__main__':
    main()
