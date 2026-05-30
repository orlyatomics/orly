#!/usr/bin/env python3
# Copyright 2010-2026 Atomic Kismet Company
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import argparse
import itertools
import multiprocessing
import os
import os.path
import pickle
import subprocess

# Process object. wraps subprocess.Popen object to augment it with a filename.
class TProcess(object):
  def __init__(self, filepath, process):
    self.filepath = filepath
    self.process = process
    self.pid = process.pid
    self.stdout = process.stdout

  def poll(self):
    return self.process.poll()

  def returncode(self):
    return self.process.returncode

# Bounded worker pool for orlyc invocations. Replaces the original
# littleworkers.Pool dependency, which is unmaintained and has a
# Python 3 typing bug in its poll loop.
class TPool(object):
    def __init__(self, out_dir, workers):
        self.__out_dir = out_dir
        self.__workers = max(1, int(workers))

    def create_process(self, filepath):
        return TProcess(
                 filepath,
                 subprocess.Popen(
                     '../out_orly/debug/orly/orlyc -m -d ' + filepath + ' -o ' + self.__out_dir,
                     shell=True,
                     stderr=subprocess.STDOUT,
                     stdout=subprocess.PIPE))

    def run(self, filepaths, callback):
        import time
        pending = list(filepaths)
        in_flight = []  # list of TProcess
        while pending or in_flight:
            while pending and len(in_flight) < self.__workers:
                in_flight.append(self.create_process(pending.pop(0)))
            # Reap any that finished. Polling rather than os.wait so we
            # don't race subprocess.Popen's own bookkeeping; it would
            # otherwise see ECHILD and set returncode to 0 incorrectly.
            still_running = []
            made_progress = False
            for proc in in_flight:
                if proc.poll() is None:
                    still_running.append(proc)
                else:
                    callback(proc)
                    made_progress = True
            in_flight = still_running
            if not made_progress and in_flight:
                time.sleep(0.05)

def GetArgParser():
  default_worker_count = multiprocessing.cpu_count()

  # Parse command line arguments
  parser = argparse.ArgumentParser(description='Report changes in tests')
  parser.add_argument('filepaths', type=str, nargs='*',
      help='The list of orlyscript files to use, defaults to all *.orly files in the current directory')
  parser.add_argument('--update', '-u', action='store_true', help='Update the state files for the given orlyscript files')
  parser.add_argument('--changes', '-c', action='store_true', help='Print out the changes')
  parser.add_argument('--directories', '-d', action='store_true', help='Treat filepaths as directories containing files rather than files');
  parser.add_argument(
      '--worker-count', '-w',
      default=default_worker_count,
      type=int,
      help='Specify the number of workers for the script. (default: ' + str(default_worker_count) + ')')
  return parser

def GetResult(proc):
  output = proc.stdout.read()
  if isinstance(output, bytes):
    output = output.decode('utf-8', errors='replace')
  sections = [x.splitlines() for x in output.split('MM_NOTICE: ')]
  return proc.returncode(), sections

def GetStateFilename(filepath):
  dirpath, filename = os.path.split(filepath)
  return os.path.join(dirpath, '.' + filename + '.test.state')

def Main():
  args = GetArgParser().parse_args()

  # We should use mkdtemp, but that creates a new directory name for every build, giving us false positives for output changing.
  # out_dir = tempfile.mkdtemp(prefix = "orly") # Temp Directory
  out_dir = '/tmp/orly_compiler/'
  try:
    os.mkdir(out_dir)
  except OSError:
    pass

  # Singleton instance of our process pool
  lil = TPool(out_dir, workers=args.worker_count)



  # List of filepaths.
  filepaths = list();

  if args.directories:
    for path in args.filepaths:
      for dirname, _subdirs, filenames in os.walk(path):
        for filename in filenames:
          if os.path.splitext(filename)[-1] == '.orly':
            filepaths.append(os.path.join(dirname, filename))
  else:
    filepaths = args.filepaths

  changed_files = []
  passed_files = []
  failed_files = []

  def Callback(proc):
    # Get the result
    result = GetResult(proc)
    filepath = proc.filepath
    if args.update:
      with open(GetStateFilename(filepath), 'wb') as f:
        pickle.dump(result, f)
      return
    else:
      try:
        with open(GetStateFilename(filepath), 'rb') as f:
          expected = pickle.load(f)
      except (IOError, OSError) as ex:
        if ex.errno != 2:
          raise
        if result[0] != 0:
          print('New failure:', filepath)
          print('Exited with code', result[0])
          print('OUTPUT: ')
          print('\n'.join('\n'.join(x) for x in result[1]))
          changed_files.append(filepath)
          failed_files.append(filepath)
        else:
          passed_files.append(filepath)
        return

      # Did we pass?
      passed = result[0] == 0

      if result == expected:
        if passed:
          passed_files.append(filepath)
        else:
          failed_files.append(filepath)
        return

      returncode, section = result
      expected_returncode, expected_section = expected

      print('====================================================================')
      print('Changes:', filepath)

      acceptable_change = True

      def _label(section_val):
        return section_val[0] if section_val else '(empty)'

      for (expected_val, val) in itertools.zip_longest(expected_section, section):
        if expected_val == val:
          continue
        elif expected_val is None:
          acceptable_change = False
          print('Further:', _label(val))
          if args.changes:
            print('\n'.join(val))
        elif val is None:
          acceptable_change = False
          print('Worse:', _label(expected_val))
        else:
          print('Differs:', _label(expected_val))
          if args.changes:
            print('Old:')
            if len(expected_val) > 0:
              print('\n'.join(expected_val[1:]))
            print('New:')
            if len(val) > 0:
              print('\n'.join(val[1:]))

      if returncode != expected_returncode:
        acceptable_change = False
        print('Return code from', expected_returncode, 'to', returncode)

      if not acceptable_change:
        changed_files.append(filepath)

      if passed:
        passed_files.append(filepath)
      else:
        failed_files.append(filepath)
      return

  if len(filepaths) != 0:
    lil.run(filepaths, callback=Callback)

  if args.update:
    return 0

  print('Change:', ','.join(changed_files))
  print('Pass:', ','.join(passed_files))
  print('Fail:', ','.join(failed_files))
  print('Overall: %s changed, %s passed, %s failed' % (len(changed_files), len(passed_files), len(failed_files)))
  return 0 if len(changed_files) == 0 else -1

if __name__ == '__main__':
  exit(Main())
