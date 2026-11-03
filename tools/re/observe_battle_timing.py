"""Read-only Windows observer for the identified CUSA01593 01.02 timing fields.

Select only a local test runtime PID. Reports scalar counters without heap
addresses, local paths, usernames or memory dumps. An unavailable battle object
is reported as null. This diagnostic is not a framerate patch.
"""
import argparse
import ctypes as c
import json
import math
import pathlib
import struct
import time


class Observer:
    def __init__(self, pid, base):
        self.base = base
        self.k = c.WinDLL('kernel32', use_last_error=True)
        self.k.OpenProcess.argtypes = [c.c_uint32,c.c_int,c.c_uint32]
        self.k.OpenProcess.restype = c.c_void_p
        self.k.ReadProcessMemory.argtypes = [c.c_void_p,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
        self.k.CloseHandle.argtypes = [c.c_void_p]
        self.handle = self.k.OpenProcess(0x1010,False,pid)
        if not self.handle:
            raise c.WinError(c.get_last_error())
        try:
            # Verify the rate getter before following title-specific pointers.
            if self.read(base+0x404360,10) != bytes.fromhex('488d055d453a008b00c3'):
                raise ValueError('unsupported image or image base')
        except Exception:
            self.close()
            raise

    def close(self):
        if self.handle:
            self.k.CloseHandle(self.handle)
            self.handle = None

    def read(self, address, size):
        buf=c.create_string_buffer(size)
        got=c.c_size_t()
        if not self.k.ReadProcessMemory(self.handle,address,buf,size,c.byref(got)) or got.value!=size:
            raise c.WinError(c.get_last_error())
        return buf.raw

    def ptr(self, address):
        value=struct.unpack('<Q',self.read(address,8))[0]
        if value and not 0x800000000 <= value < 0x10000000000:
            raise ValueError('pointer is outside the guest allocation window')
        return value

    def snapshot(self):
        rate_f,rate_i=struct.unpack('<fI',self.read(self.base+0x7a88c0,8))
        frames,ticks,step=struct.unpack('<3I',self.read(self.base+0x81f370,12))
        battle=None
        try:
            manager=self.ptr(self.base+0x7e6f40)
            array=self.ptr(manager+8)
            if not array:
                array=self.ptr(self.ptr(manager)+0x88)
            timer=self.ptr(array+0x80)  # child type 15, (type+1)*8
            vtable=struct.unpack('<Q',self.read(timer,8))[0]
            if vtable != self.base+0x7b99f0:
                raise ValueError('timer vtable does not match')
            data=self.read(timer+0x18,0x40)
            values=struct.unpack('<16I',data)
            battle={'limit_frames':values[0], 'limit_play_frames':values[1],
                    'elapsed_frames':values[6], 'elapsed_play_frames':values[7],
                    'remaining_frames':values[12], 'remaining_play_frames':values[13],
                    'display_seconds':values[14]}
        except (OSError,ValueError):
            pass
        return {'engine_rate_float':rate_f,'engine_rate_int':rate_i,
                'frame_counter':frames,'tick_counter':ticks,'last_step':step,'battle':battle}


def sample_rows(obs, seconds, interval):
    """Include the initial sample, then stop without sleeping past the deadline."""
    if not math.isfinite(seconds) or seconds < 0:
        raise ValueError('duration must be finite and nonnegative')
    if not math.isfinite(interval) or interval <= 0:
        raise ValueError('interval must be finite and positive')
    start = time.monotonic()
    while True:
        yield {'wall_seconds': round(time.monotonic()-start, 6), **obs.snapshot()}
        remaining = seconds - (time.monotonic()-start)
        if remaining <= 0:
            return
        time.sleep(min(interval, remaining))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid',type=int,required=True)
    p.add_argument('--base',type=lambda v:int(v,0),default=0x200000000)
    p.add_argument('--seconds',type=float,default=30)
    p.add_argument('--interval',type=float,default=1)
    p.add_argument('--out',type=pathlib.Path,required=True)
    args=p.parse_args()
    if not math.isfinite(args.seconds) or args.seconds<0 or not math.isfinite(args.interval) or args.interval<=0:
        p.error('duration must be finite and nonnegative and interval finite and positive')
    obs=Observer(args.pid,args.base)
    try:
        with args.out.open('x',encoding='utf-8') as out:
            for row in sample_rows(obs, args.seconds, args.interval):
                out.write(json.dumps(row)+'\n');out.flush()
                print(json.dumps(row),flush=True)
    finally:
        obs.close()


if __name__=='__main__':
    main()
