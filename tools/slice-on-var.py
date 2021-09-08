#!/usr/bin/env python3

import os, sys, warnings
from typing import Dict, List, Set
import subprocess
import csv
import argparse
import pickle

parser = argparse.ArgumentParser('Python script to slice the program based on given variables.')
parser.add_argument(
    'bc_file', 
    type=str,
    help='bytecode file from llvm-link'
    )
parser.add_argument(
    '-f',
    dest='var_file',
    nargs=1,
    type=str,
    help='csv file with headers [file, line, varname, entry]'
)
parser.add_argument(
    '--forward',
    '-fw',
    action='store_true'
)
parser.add_argument(
    '--bidirection',
    '-bd',
    action='store_true'
)
parser.add_argument(
    '-sc',
    nargs=1,
    type=str,
)
parser.add_argument(
    '-entry',
    nargs=1,
    type=str,
)

parser.add_argument(
    '--print',
    '-p',
    action='store_true'
)
parser.add_argument(
    '--dbg',
    action='store_true'
)

debug = False

def run_slicer(bc_file:str, crit:dict, entry:str, forward:bool=False, linenum=False) -> str:
    # In criteria, file, line number, variable name is mandatory. 
    # Function name is optional
    # The llvm-src-slicer needs to be in the PATH

    file = crit['file']
    line = crit['line']
    varname = crit['varname']
    fname = crit['fname'] if 'fname' in crit.keys() else ""

    crit_cl = file + '#' + fname + '#' + line + '#&' + varname

    cmd = ["llvm-src-slicer", "-sc", crit_cl, "-entry", entry]
    if not debug:
            cmd += ["--dont-verify"]
    if forward:
        cmd += ["--forward"]
    if linenum:
        cmd += ["--linenum"]
    cmd += [bc_file]

    try:
        ret = subprocess.run(cmd, capture_output=True, text=True)
    except FileNotFoundError as err:
        print(err, file=sys.stderr)
        print('Please check the arguments and make sure llvm-src-slicer is in the PATH.', file=sys.stderr)
        exit(-1)

    if ret.returncode != 0:
        raise RuntimeError(ret.stderr)
    
    return ret.stdout


def test():
    #variable_file = pd.read_csv(argv[1])
    print(
        run_slicer(
            "bftpd.bc", 
            {
                "file": "commands.c",
                "line": "141",
                "varname": "foo"
            },
            "command_stor"
        )
    )

def do_slice(bc_file, sc=None, forward=None, entry=None, var_file=None, linenum=False):
    if entry is None:
        entry = 'main'

    code_slices: List[str] = list()
    labels: List[str] = list()
    
    if var_file is not None:
        with open(var_file[0], 'r') as f:
            vars = csv.reader(f)
            for var in vars:
                # If no label
                # csv columns: [file, line, varname, entry]
                # If with label
                # csv columns: [file, line, varname, entry, label]
                if os.path.isfile(var[0]) and var[1].isnumeric(): # Simple check
                    code_slices.append(run_slicer(
                            bc_file,
                            {
                                "file": var[0],
                                "line": var[1],
                                "varname": var[2]
                            },
                            var[3],
                            forward,
                            linenum=linenum
                        )
                    )
                if len(var) == 5:
                    labels.append(var[4])

        if len(labels) > 0 and len(code_slices) != len(labels):
            raise RuntimeError('Number of labels and code slices do not match.')
    
    elif sc is not None:
        cmd = ["llvm-src-slicer", "-sc", sc[0]]
        if not debug:
            cmd += ["--dont-verify"]
        if entry is not None:
            cmd += ["-entry", entry[0]]
        if forward:
            cmd += ["--forward"]
        if linenum:
            cmd += ["--linenum"]
        cmd += [bc_file]

        try:
            ret = subprocess.run(cmd, capture_output=True, text=True)
        except FileNotFoundError as err:
            print(err, file=sys.stderr)
            print('Please check the arguments and make sure llvm-src-slicer is in the PATH.', file=sys.stderr)
            exit(-1)

        if ret.returncode != 0:
            raise RuntimeError(ret.stderr)

        code_slices.append(ret.stdout)
    
    else:
        raise RuntimeError("Internal Error: No sc or var_file")

    return {'code_slices': code_slices, 'labels': labels}

def bidirectional_slice(bc_file, sc=None, entry=None, var_file=None):
    # Note: Due to LLVM engine global state issue, we call llvm-src-slicer for backward and forward slicing seperately
    # and ask to provide filenames and line numbers. Then we fetch the source code here
    result_b = do_slice(bc_file, sc=sc, forward=False, entry=entry, var_file=var_file, linenum=True)
    result_f = do_slice(bc_file, sc=sc, forward=True, entry=entry, var_file=var_file, linenum=True)

    code_slices = list()

    for b, f in zip(result_b['code_slices'], result_f['code_slices']):
        slice_dict:Dict[str, Set[int]] = dict()

        b = b.split('\n')
        f = f.split('\n')

        # Check backward
        for bf in b:
            bf = bf.split(',')

            if bf[0] not in slice_dict.keys():
                slice_dict[bf[0]] = set()
            for i in range(1, len(bf)):
                slice_dict[bf[0]].add(int(bf[i]))

        # Check forward
        for ff in f:
            ff = ff.split(',')

            if ff[0] not in slice_dict.keys():
                slice_dict[ff[0]] = set()
            for i in range(1, len(ff)):
                slice_dict[ff[0]].add(int(ff[i]))

        src_code = str()

        for file in slice_dict.keys():
            if file == '':
                continue
            with open(file, 'r') as f:
                for l, line in enumerate(f):
                    if l + 1 in slice_dict[file]: # Line number start from 1
                        src_code += line
                        # src_code += '\n'
            
        code_slices.append(src_code)


        # for k in slice_dict.keys():
        #     print(k, slice_dict[k])

    return {'code_slices': code_slices, 'labels': result_f['labels']}  

    

def main():
    args = parser.parse_args()
    forward:bool = args.forward
    bc_file:str = args.bc_file
    var_file:str = args.var_file
    sc:str = args.sc
    entry:str = args.entry
    _print:bool = args.print
    bidirection:bool = args.bidirection
    if args.dbg:
        global debug
        debug = True

    if var_file is not None and (sc is not None or entry is not None):
        warnings.warn('var_file present. -sc and -entry is ignored', RuntimeWarning)

    if var_file is None and sc is None:
        raise RuntimeError('Either var file or -sc criteria need to be provided.')

    if var_file is not None and _print:
        warnings.warn('Cannot print when var_file provided. Dump to file.', RuntimeWarning)
        _print = False

    if bidirection and forward:
        warnings.warn('Bidirection flag given. --forward flag ignored.', RuntimeWarning)

    if bidirection:
        pkl_dict = bidirectional_slice(bc_file, sc=sc, entry=entry, var_file=var_file)
    else:
        pkl_dict = do_slice(bc_file, sc=sc, forward=forward, entry=entry, var_file=var_file)

    if not _print:
        with open('sliced_data.pkl', 'wb') as pklfile:
            pickle.dump(pkl_dict, pklfile)
    else:
        print(pkl_dict['code_slices'][0])

if __name__ == "__main__":
    main()

    
        
