#!/usr/bin/env python3
"""Opt-in physical test helper. No flashing, resets, cloud writes, or raw pixel logs."""
import argparse, glob, json, time
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--calibrate-empty', action='store_true')
    parser.add_argument('--seconds', type=int, default=300)
    args = parser.parse_args()
    import serial
    ports = sorted({p for pattern in ('/dev/cu.usbserial*', '/dev/cu.usbmodem*', '/dev/cu.SLAB_USBtoUART*', '/dev/cu.wchusbserial*', '/dev/cu.CP210*') for p in glob.glob(pattern)})
    if len(ports) != 1:
        raise SystemExit('Connect only the spare thermal ESP32, then reopen this command.')
    if args.calibrate_empty:
        print('This records a NEW empty-scene background in RAM. Keep the camera fixed and let it settle first.')
        input('Leave the entire scene empty, then press Return to confirm. ')
    root = Path(__file__).resolve().parents[1]
    path = root/'evidence'/time.strftime('physical-test-%Y%m%d-%H%M%S.jsonl')
    connection = serial.Serial(port=None, baudrate=115200, timeout=1, exclusive=True)
    connection.dtr = False; connection.rts = False; connection.port = ports[0]
    try:
        connection.open()
    except serial.SerialException:
        raise SystemExit('USB port is busy or unavailable. Close other serial viewers and retry.')
    started = time.monotonic(); previous = None; count = 0; sent = False
    if not args.calibrate_empty:
        print('The self-learning candidate learns automatically; empty-scene confirmation is optional.')
    learning = None
    print('Recording local AI diagnostics for', args.seconds, 'seconds. No raw images are saved.')
    print('Output:', path)
    allowed = {'event','bootId','captureStartMs','captureEndMs','captureSeq','readUs','featureUs','maxReadUs','maxFeatureUs','valid','calibrated','continuous','regions','ambiguous','tracked','dwellMs','stationaryCandidate','area','cx','cy','bbox','min_c','max_c','mean_c','change_c','errors','gaps','person','posture','fall','bodyTemperature'}
    with connection, path.open('w') as output:
        while time.monotonic()-started < args.seconds:
            line = connection.read_until(b'\n', size=4096)
            try: data = json.loads(line)
            except (ValueError, UnicodeError): continue
            if data.get('event') == 'thermal_learning_v1':
                keys = {'event','baselineReady','emptySceneConfirmed','baselineConfidence','noiseReady','learningState','learnedNoise_c','learnedDeviation_c','stillnessThreshold_c','regionChange_c','learningWindows','frozenWindows','downweightedWindows','candidateNoise_c','noiseBackgroundPixels','noiseEligible'}
                learning = {k:v for k,v in data.items() if k in keys}
                learning['receivedAt'] = time.strftime('%Y-%m-%dT%H:%M:%S%z')
                output.write(json.dumps(learning)+'\n'); output.flush()
                continue
            if data.get('event') != 'thermal_local_v1': continue
            if args.calibrate_empty and not sent:
                connection.write(b'THERMAL CALIBRATE EMPTY\n'); connection.flush(); sent = True
                print('Empty-scene calibration requested; keep the scene empty until calibrated=True.')
            entry = {k:v for k,v in data.items() if k in allowed}
            entry['receivedAt'] = time.strftime('%Y-%m-%dT%H:%M:%S%z')
            hz = None
            if previous and previous.get('bootId') == data.get('bootId'):
                dt = (data['captureEndMs']-previous['captureEndMs']) & 0xffffffff
                ds = (data['captureSeq']-previous['captureSeq']) & 0xffffffff
                if 0 < dt < 10000 and ds < 1000: hz = ds*1000/dt
            entry['observedCaptureHz'] = hz
            if learning:
                entry['baselineReady'] = learning.get('baselineReady')
                entry['noiseReady'] = learning.get('noiseReady')
                entry['learningState'] = learning.get('learningState')
            output.write(json.dumps(entry)+'\n'); output.flush(); previous = data; count += 1
            print('captureHz=%s emptyConfirmed=%s regions=%s tracked=%s dwell=%.1fs errors=%s gaps=%s' %
                  ('—' if hz is None else '%.2f'%hz, data.get('calibrated'), data.get('regions'), data.get('tracked'), data.get('dwellMs',0)/1000, data.get('errors'), data.get('gaps')), flush=True)
            if learning and count % 5 == 0:
                print('Automatic learning: scene=%s noiseReady=%s state=%s threshold=%.3fC backgroundPixels=%s eligible=%s' % (learning.get('baselineConfidence'), learning.get('noiseReady'), learning.get('learningState'), learning.get('stillnessThreshold_c',0), learning.get('noiseBackgroundPixels'), learning.get('noiseEligible')))
    if not count:
        print('No candidate diagnostics received. v0.3.0 does not emit these; no calibration command was sent.')
    else:
        print('Recorded', count, 'diagnostic samples. Human/posture/fall accuracy remains unvalidated.')
if __name__ == '__main__':
    try: main()
    except KeyboardInterrupt: print('\nRecording stopped.')
