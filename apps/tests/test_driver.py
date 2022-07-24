# Copyright (c) 2022 Marcin Zdun
# This code is licensed under MIT license (see LICENSE for details)

import argparse
import json
import os
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
from difflib import unified_diff

if os.name == 'nt':
    from ctypes import create_unicode_buffer, windll
    GetLongPathName = windll.kernel32.GetLongPathNameW

    sys.stdout.reconfigure(encoding='utf-8')

flds = ['Return code', 'Standard out', 'Standard err']
streams = ['stdin', 'stderr']

parser = argparse.ArgumentParser()
parser.add_argument("--target", required=True, metavar="EXE")
parser.add_argument("--tests", required=True, metavar="DIR")
parser.add_argument("--data-dir", required=True, metavar="DIR")
parser.add_argument("--version", required=True, metavar="SEMVER")
parser.add_argument("--install", metavar="DIR")
parser.add_argument("--install-with", metavar="TOOL",
                    type=lambda s: s.split(';'), action='append', default=[])
parser.add_argument("--run", metavar="TEST[,...]",
                    type=lambda s: [int(x) for x in s.split(',')], action='append', default=[])
args = parser.parse_args()
args.install_with = [item for groups in args.install_with for item in groups]
args.run = [item for groups in args.run for item in groups]
args.data_dir = os.path.abspath(args.data_dir).replace('\\', '/')
args.data_dir_alt = None

target = args.target
target_name = os.path.basename(target)
version = args.version

TEMP = os.path.abspath(os.path.join(
    tempfile.gettempdir(), "test-driver")).replace('\\', '/')
TEMP_ALT = None

os.makedirs(TEMP, exist_ok=True)

if os.name == 'nt':
    BUFFER_SIZE = 2048
    buffer = create_unicode_buffer(BUFFER_SIZE)
    GetLongPathName(TEMP, buffer, BUFFER_SIZE)
    TEMP = buffer.value

if os.sep != '/':
    TEMP_ALT = TEMP.replace('/', os.sep)
    args.data_dir_alt = args.data_dir.replace('/', os.sep)
print(TEMP_ALT, TEMP)


def expand(input):
    return (
        input.replace('$TMP', TEMP)
        .replace('$DATA', args.data_dir)
        .replace('$VERSION', args.version)
    )


def alt_sep(input, value, var):
    split = input.split(value)
    first = split[0]
    split = split[1:]
    for index in range(len(split)):
        m = re.match(r'(\S+)(\s*.*)', split[index])
        g2 = m.group(2)
        if g2 is None:
            g2 = ''
        split[index] = '{}{}'.format(m.group(1).replace(os.sep, '/'), g2)
    return var.join([first, *split])


def fix(input, patches):
    if os.name == 'nt':
        input = input.replace(b'\r\n', b'\n')
    input = input.decode('UTF-8')
    input = alt_sep(input, TEMP, '$TMP')
    input = alt_sep(input, args.data_dir, '$DATA')
    input = input.replace(args.version, '$VERSION')

    if TEMP_ALT is not None:
        input = alt_sep(input, TEMP_ALT, '$TMP')
        input = alt_sep(input, args.data_dir_alt, '$DATA')

    if not len(patches):
        return input

    input = input.split('\n')
    for patch in patches:
        patched = patches[patch]
        ptrn = re.compile(patch)
        for lineno in range(len(input)):
            if ptrn.match(input[lineno]):
                input[lineno] = patched
    return '\n'.join(input)


def last_enter(s):
    if len(s) and s[-1] == '\n':
        s = s[:-1] + '\\n'
    return s + '\n'


def diff(expected, actual):
    expected = last_enter(expected).splitlines(keepends=True)
    actual = last_enter(actual).splitlines(keepends=True)
    return ''.join(list(unified_diff(expected, actual))[2:])


def touch(args):
    os.makedirs(os.path.dirname(args[0]), exist_ok=True)
    with open(args[0], "wb") as f:
        if len(args) > 1:
            f.write(args[1].encode('UTF-8'))


def git(args):
    subprocess.run(["git", *args], shell=False)


def cov(aditional):
    subprocess.run([args.target, *aditional], shell=False)


file_cache = {}
rw_mask = stat.S_IWRITE | stat.S_IWGRP | stat.S_IWOTH
ro_mask = 0o777 ^ rw_mask


def make_RO(args):
    print(f'{args}...')
    mode = os.stat(args[0]).st_mode
    print('{:03o} -> {:03o}'.format(mode, mode & ro_mask))
    file_cache[args[0]] = mode
    os.chmod(args[0], mode & ro_mask)
    print('{:03o} -> {:03o}'.format(mode, os.stat(args[0]).st_mode))


def make_RW(args):
    try:
        mode = file_cache[args[0]]
    except KeyError:
        mode = os.stat(args[0]).st_mode | rw_mask
    os.chmod(args[0], mode)


op_types = {
    'mkdirs': (1, lambda args: os.makedirs(expand(args[0]), exist_ok=True)),
    'rm': (1, lambda args: shutil.rmtree(expand(args[0]))),
    'ro': (1, make_RO),
    'rw': (1, make_RW),
    'touch': (1, touch),
    'cd': (1, lambda args: os.chdir(expand(args[0]))),
    'git': (0, git),
    'cov': (0, cov),
}


class Test:
    def __init__(self, data, filename, count):
        self.data = data
        self.ok = True

        renovate = False

        name = os.path.splitext(os.path.basename(filename))[0].split('-')
        if int(name[0]) == count:
            name = name[1:]
        else:
            name[0] = '({})'.format(name[0])
        self.name = ' '.join(name)

        try:
            call_args, expected = data['args'], data['expected']
            if isinstance(call_args, str):
                call_args = shlex.split(call_args)
            else:
                data['args'] = shlex.join(call_args)
                renovate = True
            self.args = call_args
            self.expected = expected
        except KeyError:
            self.ok = False
            return

        if self.expected is not None:
            if not isinstance(self.expected[1], str):
                self.expected[1] = '\n'.join(self.expected[1])
            if not isinstance(self.expected[2], str):
                self.expected[2] = '\n'.join(self.expected[2])

        try:
            self.patches = data['patches']
        except KeyError:
            self.patches = {}

        self.check = ["all", "all"]
        for stream in range(len(streams)):
            try:
                self.check[stream] = data['check'][streams[stream]]
            except KeyError:
                pass

        try:
            self.prepare = data['prepare']
            for cmd in self.prepare:
                if not isinstance(cmd, str):
                    for index in range(len(data['prepare'])):
                        if not isinstance(data['prepare'][index], str):
                            data['prepare'][index] = shlex.join(
                                data['prepare'][index])
                    renovate = True
                    break
        except KeyError:
            self.prepare = []

        try:
            self.cleanup = data['cleanup']
            for cmd in self.cleanup:
                if not isinstance(cmd, str):
                    for index in range(len(data['cleanup'])):
                        if not isinstance(data['cleanup'][index], str):
                            data['cleanup'][index] = shlex.join(
                                data['cleanup'][index])
                    renovate = True
                    break
        except KeyError:
            self.cleanup = []

        try:
            self.lang = data['lang']
        except KeyError:
            self.lang = 'en'

        try:
            self.env = data['env']
        except KeyError:
            self.env = []

        if renovate:
            data['expected'] = [self.expected[0], *
                                [to_lines(stream) for stream in self.expected[1:]]]
            with open(filename, "w") as f:
                json.dump(data, f, indent=4)
                print(file=f)

    @ staticmethod
    def run_cmds(ops):
        for op in ops:
            orig = op
            if isinstance(op, str):
                op = shlex.split(op)
            is_safe = False
            try:
                name = op[0]
                if name[:5] == 'safe-':
                    name = name[5:]
                    is_safe = True
                min_args, cb = op_types[name]
                op = op[1:]
                if len(op) < min_args:
                    return None
                cb([expand(o) for o in op])
            except Exception as ex:
                if op[0] != 'safe-rm':
                    print('Problem while handling', orig)
                    print(ex)
                if is_safe:
                    continue
                return None
        return True

    def run(self):
        current_directory = os.getcwd()

        prep = Test.run_cmds(self.prepare)
        if prep is None:
            return None

        expanded = [expand(arg) for arg in self.args]

        env = {name: os.environ[name] for name in os.environ}
        env['LANGUAGE'] = self.lang
        for key in self.env:
            value = self.env[key]
            if value:
                env[key] = expand(value)
            elif key in env:
                del env[key]

        proc = subprocess.run([target, *expanded],
                              capture_output=True, env=env)

        clean = Test.run_cmds(self.cleanup)
        if clean is None:
            return None

        os.chdir(current_directory)

        return [
            proc.returncode,
            fix(proc.stdout, self.patches),
            fix(proc.stderr, self.patches),
        ]

    def clip(self, actual):
        clipped = [*actual]
        for ndx in range(len(self.check)):
            check = self.check[ndx]
            if check != "all":
                if check == "begin":
                    clipped[ndx + 1] = clipped[ndx +
                                               1][:len(self.expected[ndx + 1])]
                elif check == "end":
                    clipped[ndx + 1] = clipped[ndx +
                                               1][-len(self.expected[ndx + 1]):]
                else:
                    return check
        return clipped

    def report(self, actual):
        for ndx in range(len(actual)):
            if actual[ndx] == self.expected[ndx]:
                continue
            if ndx:
                check = self.check[ndx - 1]
                pre_mark = '...' if check == "end" else ''
                post_mark = '...' if check == "begin" else ''
                print(f"""{flds[ndx]}
  Expected:
    {pre_mark}{repr(self.expected[ndx])}{post_mark}
  Actual:
    {pre_mark}{repr(actual[ndx])}{post_mark}

Diff:
{diff(self.expected[ndx], actual[ndx])}""")
            else:
                print(f"""{flds[ndx]}
  Expected:
    {repr(self.expected[ndx])}
  Actual:
    {repr(actual[ndx])}""")

        env = {}
        env['LANGUAGE'] = self.lang
        for key in self.env:
            value = self.env[key]
            if value:
                env[key] = value
            elif key in env:
                del env[key]

        expanded = [expand(arg) for arg in self.args]
        print(' '.join(shlex.quote(arg) for arg in [
              *['{}={}'.format(key, env[key]) for key in env], target, *expanded]))

    @ staticmethod
    def load(filename, count):
        with open(filename) as f:
            return Test(json.load(f), filename, count)


if args.install is not None:
    root_dir = os.path.dirname(os.path.dirname(target))

    os.makedirs(os.path.join(args.install, 'bin'), exist_ok=True)
    os.makedirs(os.path.join(args.install, 'libexec', 'cov'), exist_ok=True)

    shutil.copy2(target, os.path.join(args.install, 'bin'))
    if os.path.exists(os.path.join(root_dir, "libexec")):
        shutil.copytree(os.path.join(root_dir, "libexec"), os.path.join(
            args.install, 'libexec'), dirs_exist_ok=True)
    if os.path.exists(os.path.join(root_dir, "share")):
        shutil.copytree(os.path.join(root_dir, "share"), os.path.join(
            args.install, 'share'), dirs_exist_ok=True)

    for module in args.install_with:
        shutil.copy2(module, os.path.join(args.install, 'libexec', 'cov'))

    target = os.path.join(args.install, 'bin', target_name)


print(target)
print(args.data_dir)
print(args.tests)
print('version:', args.version)

testsuite = []
for root, dirs, files in os.walk(args.tests):
    for filename in files:
        testsuite.append(os.path.join(root, filename))

length = len(testsuite)
digits = 1
while length > 9:
    digits += 1
    length = length // 10


def to_lines(stream):
    lines = stream.split('\n')
    if len(lines) > 1 and lines[-1] == '':
        lines = lines[:-1]
        lines[-1] += '\n'
    if len(lines) == 1:
        return lines[0]
    return lines


class color:
    reset = "\033[m"
    counter = "\033[2;49;92m"
    name = "\033[0;49;90m"
    failed = "\033[0;49;91m"
    passed = "\033[2;49;92m"
    skipped = "\033[0;49;34m"


counter = 0
error_counter = 0
skip_counter = 0
save_counter = 0
run = args.run
if not len(run):
    run = list(range(1, len(testsuite)+1))
else:
    print("running:", ', '.join(str(x) for x in run))
for filename in sorted(testsuite):
    counter += 1
    if counter not in run:
        continue

    test = Test.load(filename, counter)
    if not test.ok:
        continue

    print(
        f"{color.counter}[{counter:>{digits}}/{len(testsuite)}]{color.reset} {color.name}{test.name}{color.reset}"
    )

    actual = test.run()
    if actual is None:
        print(
            f"{color.counter}[{counter:>{digits}}/{len(testsuite)}]{color.reset} {color.name}{test.name}{color.reset} {color.skipped}SKIPPED{color.reset}"
        )
        skip_counter += 1
        continue

    if test.expected is None:
        test.data['expected'] = [actual[0], *
                                 [to_lines(stream) for stream in actual[1:]]]
        with open(filename, "w") as f:
            json.dump(test.data, f, indent=4)
            print(file=f)
        print(
            f"{color.counter}[{counter:>{digits}}/{len(testsuite)}]{color.reset} {color.name}{test.name}{color.reset} {color.skipped}saved{color.reset}"
        )
        skip_counter += 1
        save_counter += 1
        continue

    clipped = test.clip(actual)

    if isinstance(clipped, str):
        print(
            f"{color.counter}[{counter:>{digits}}/{len(testsuite)}]{color.reset} {color.name}{test.name}{color.reset} {color.failed}FAILED (unknown check '{clipped}'){color.reset}"
        )
        error_counter += 1
        continue

    if actual == test.expected or clipped == test.expected:
        print(
            f"{color.counter}[{counter:>{digits}}/{len(testsuite)}]{color.reset} {color.name}{test.name}{color.reset} {color.passed}PASSED{color.reset}"
        )
        continue

    test.report(clipped)
    print(
        f"{color.counter}[{counter:>{digits}}/{len(testsuite)}]{color.reset} {color.name}{test.name}{color.reset} {color.failed}FAILED{color.reset}"
    )
    error_counter += 1


if args.install is not None:
    shutil.rmtree(args.install)
shutil.rmtree(TEMP, ignore_errors=True)

print(f"Failed {error_counter}/{counter}")
if skip_counter > 0:
    skip_test = "test" if skip_counter == 1 else "tests"
    if save_counter > 0:
        print(
            f"Skipped {skip_counter} {skip_test} (including {save_counter} due to saving)")
    else:
        print(f"Skipped {skip_counter} {skip_test}")

if error_counter:
    sys.exit(1)
