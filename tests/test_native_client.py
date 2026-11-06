# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("native_client",ROOT/"tools/native_modem.py")
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class FakeLink:
    """Protocol/transport test double; never emulates an RF waveform."""
    def __init__(self,fail=None):
        self.r={f:0 for f in m.FIELDS};self.r["version"]=1
        self.ops=[];self.loaded=[];self.queue=[];self.capture=[];self.fail=fail;self.failed=False
    def request(self,cmd,payload,busy_wait):
        assert cmd==0x21 and busy_wait==0
        op=payload[0];self.ops.append(op)
        if self.fail=="disconnect" and op==3: self.failed=True;raise TimeoutError("cable disconnected")
        if self.fail=="disconnect" and self.failed: raise TimeoutError("still disconnected")
        if self.fail=="interrupt" and op==0 and not self.failed: self.failed=True;raise KeyboardInterrupt()
        r=self.r
        if op==1:
            mode,sync,count,duration,lease,hz,token=struct.unpack("<BBHHHII",payload[1:])
            assert mode!=2 or token==0x3532504e
            r.update(mode=mode,p25_sync=sync,blocks=count,duration_ms=duration,lease_ms=lease,frequency_hz=hz,state=1,session=77)
            return 0,struct.pack("<I",77)
        if op==0:
            if r["state"] in (2,3):
                if self.fail=="fault": r.update(state=6,reason=10)
                elif r["mode"]==2:
                    assert self.queue
                    self.queue.pop(0);r["tx_sent"]+=1;r["tx_count"]=len(self.queue)
                    if r["tx_sent"]==r["blocks"]: r.update(state=4)
                else:
                    seq=r["rx_seen"];r["rx_seen"]+=1;r["rx_saved"]+=1
                    self.capture.append(struct.pack("<IIII",seq,seq*30000,0x800,seq%3)+bytes([seq])*36)
                    r["rx_count"]=len(self.capture)
                    if r["rx_seen"]==r["blocks"]: r.update(state=5,reason=2)
            elif r["state"]==4: r.update(state=5,reason=3)
            if self.fail=="session": r["session"]+=1
            data=struct.pack("<"+"I"*len(m.FIELDS),*(r[f] for f in m.FIELDS))
            return 0,data[:-1] if self.fail=="short" else data
        sid,=struct.unpack("<I",payload[1:5])
        if sid!=r["session"]: return 7,b""
        if op==2:
            ix,=struct.unpack("<H",payload[5:7]);assert ix==len(self.loaded) and len(payload)==43
            self.loaded.append(payload[7:]);self.queue.append(payload[7:]);r["tx_loaded"]+=1;r["tx_count"]=len(self.queue)
        elif op==3:r.update(state=2 if r["mode"]==1 else 3,owned=1)
        elif op==4:
            assert r["state"] in (2,3,4)
            if self.fail in ("end_on_ping","fault_on_ping"):
                r.update(state=5 if self.fail=="end_on_ping" else 6,reason=4 if self.fail=="end_on_ping" else 5)
                return 7,b""
        elif op==5:
            r["owned"]=0;r["quiets"]=1
            if r["state"] not in (5,6):r.update(state=5,reason=1)
        elif op==6:
            b=self.capture.pop(0);r["rx_count"]=len(self.capture);r["rx_read"]+=1;return 0,b
        else:raise AssertionError(op)
        return 0,b""

class Clock:
    def __init__(self):self.t=0
    def now(self):return self.t
    def sleep(self,t):self.t+=t

class ClientTests(unittest.TestCase):
    def run_fake(self,link,mode=1,count=3):
        clock=Clock();data=m.payloads("counter",count)[0] if mode==2 else []
        return m.run_session(link,mode=mode,frequency=145500000,blocks=count,allow_rf=mode==2,
                 data=data,clock=clock.now,sleep=clock.sleep)
    def test_rx_capture_and_cleanup(self):
        f=FakeLink();r=self.run_fake(f)
        self.assertTrue(r["cleanup_confirmed"]);self.assertNotIn("error",r)
        self.assertEqual([x["sequence"] for x in r["capture"]],[0,1,2])
        self.assertEqual(r["status"]["rx_read"],3);self.assertEqual(f.ops[-2:],[5,0])
        self.assertFalse(r["rf_interoperability_verified"])
    def test_tx_refill_preserves_each_block(self):
        f=FakeLink();r=self.run_fake(f,2,16)
        self.assertEqual(f.loaded,m.payloads("counter",16)[0]);self.assertEqual(r["status"]["tx_sent"],16)
        self.assertTrue(r["cleanup_confirmed"]);self.assertNotIn("error",r)
    def test_preloaded_sample_sends_every_block_before_start_and_needs_no_heartbeat(self):
        class MissingStatus(FakeLink):
            missing=2
            def request(self,cmd,payload,busy_wait):
                if payload[0]==0 and self.r["state"]==3 and self.missing:
                    self.missing-=1
                    raise TimeoutError("status reply lost during the stored burst")
                return super().request(cmd,payload,busy_wait)
        link=MissingStatus()
        clock=Clock()
        data=m.payloads("counter",18)[0]
        result=m.run_session(link,mode=2,frequency=145500000,blocks=len(data),duration=1000,
                             lease=1000,allow_rf=True,data=data,clock=clock.now,
                             sleep=clock.sleep,tx_preload=True)
        self.assertNotIn("error",result)
        self.assertEqual(result["missed_tx_replies"],2)
        self.assertTrue(result["cleanup_confirmed"])
        self.assertEqual(link.loaded,data)
        self.assertEqual(link.ops[:len(data)+2],[1]+[2]*len(data)+[3])
        self.assertNotIn(4,link.ops)
    def test_transport_loss_never_claims_stop(self):
        f=FakeLink("disconnect");r=self.run_fake(f)
        self.assertIn(5,f.ops);self.assertIn("error",r);self.assertIn("cleanup_error",r)
        self.assertFalse(r["cleanup_confirmed"])
    def test_interrupt_always_requests_stop(self):
        f=FakeLink("interrupt");r=self.run_fake(f)
        self.assertIn(5,f.ops);self.assertIn("KeyboardInterrupt",r["error"]);self.assertTrue(r["cleanup_confirmed"])
    def test_fault_remains_latched(self):
        r=self.run_fake(FakeLink("fault"));self.assertEqual(r["status"]["reason"],10)
        self.assertEqual(r["status"]["state"],6);self.assertTrue(r["cleanup_confirmed"])
    def test_window_finishing_between_status_and_ping_is_not_a_transport_failure(self):
        r=self.run_fake(FakeLink("end_on_ping"))
        self.assertNotIn("error",r);self.assertEqual(r["status"]["reason"],4)
        self.assertTrue(r["cleanup_confirmed"]);self.assertEqual(len(r["capture"]),1)
    def test_fault_between_status_and_ping_stays_a_fault(self):
        r=self.run_fake(FakeLink("fault_on_ping"))
        self.assertEqual(r["status"]["state"],6);self.assertEqual(r["status"]["reason"],5)
        self.assertTrue(r["cleanup_confirmed"])
    def test_malformed_status_and_changed_session(self):
        for fail in ("short","session"):
            f=FakeLink(fail);r=self.run_fake(f);self.assertIn("error",r);self.assertIn(5,f.ops)
            self.assertFalse(r["cleanup_confirmed"])
    def test_pattern_and_explicit_transport_padding(self):
        data,pad=m.payloads("counter",16,b"x"*549)
        self.assertEqual(pad,27);self.assertEqual(b"".join(data),b"x"*549+bytes(27))
        self.assertEqual(m.payloads("p25-cal",2)[0],[b"\x5f"*36]*2)
        self.assertEqual(len(set(m.payloads("counter",60)[0])),60)
    def test_tx_opt_in_precedes_any_device_open(self):
        with self.assertRaisesRegex(ValueError,"allow-rf"):
            m.run_session(None,mode=2,frequency=145500000,blocks=3,data=m.payloads("counter",3)[0])
        r=subprocess.run([sys.executable,str(ROOT/"tools/native_modem.py"),"--port","/not-a-radio","run",
                          "--mode","tx","--frequency","145500000","--output","/not-written"],capture_output=True,text=True)
        self.assertEqual(r.returncode,2);self.assertIn("requires --allow-rf",r.stderr)
        self.assertNotIn("serial",r.stderr)
    def test_bounds_and_lengths(self):
        for blocks,duration,lease in [(0,2000,250),(61,2000,250),(16,500,250),(16,2001,250),(16,2000,99)]:
            with self.assertRaises(ValueError):m.validate(2,145500000,blocks,duration,lease,1,True)
        with self.assertRaises(ValueError):m.payloads("counter",1,b"")
        with self.assertRaises(ValueError):m.payloads("counter",1,b"x"*2161)
        self.assertEqual(len(m.FIELDS),37)
    def test_fixed_read_only_snapshot_layout(self):
        class Snapshot:
            def request(self,cmd,payload,busy_wait):
                self.last=cmd,payload
                return 0,struct.pack('<56I',1,*range(1,56))
        link=Snapshot();r=m.snapshot(link)
        self.assertEqual(link.last,(0x21,b'\x08'))
        self.assertEqual(r['current']['work_mode'],8)
        self.assertEqual(r['last_event_registers']['work_mode'],32)
        self.assertEqual(len(r['current']),24)

if __name__=="__main__":unittest.main()
