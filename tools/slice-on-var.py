#!/usr/bin/env python3

import os, sys, warnings
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


def run_slicer(bc_file:str, crit:dict, entry:str, forward:bool=False) -> str:
    # In criteria, file, line number, variable name is mandatory. 
    # Function name is optional
    # The llvm-src-slicer needs to be in the PATH

    file = crit['file']
    line = crit['line']
    varname = crit['varname']
    fname = crit['fname'] if 'fname' in crit.keys() else ""

    crit_cl = file + '#' + fname + '#' + line + '#&' + varname

    cmd = ["llvm-src-slicer", "-sc", crit_cl, "-entry", entry]
    if forward:
        cmd += ["--forward"]
    cmd += [bc_file]

    try:
        ret = subprocess.run(cmd, check=True, capture_output=True, text=True)
    except FileNotFoundError as err:
        print(err, file=sys.stderr)
        print('Please check the arguments and make sure llvm-src-slicer is in the PATH.', file=sys.stderr)
        exit(-1)

    if ret.returncode != 0:
        print(ret.stderr)
        raise RuntimeError('llvm-src-slicer does not exit normally.')
    
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


def main():
    args = parser.parse_args()
    forward:bool = args.forward
    bc_file:str = args.bc_file
    var_file:str = args.var_file
    sc:str = args.sc
    entry:str = args.entry
    _print:bool = args.print

    if var_file is not None and (sc is not None or entry is not None):
        warnings.warn('var_file present. -sc and -entry is ignored', RuntimeWarning)

    if var_file is None and sc is None:
        raise RuntimeError('Either bc file or -sc criteria need to be provided.')

    if var_file is not None and _print:
        warnings.warn('Cannot print when var_file provided. Dump to file.', RuntimeWarning)
        _print = False


    code_slices = list()
    labels = list()
    
    if var_file is not None:
        with open(var_file, 'r') as f:
            vars = csv.reader(f)
            for var in vars:
                if len(var) == 4:
                    # No label
                    # csv columns: [file, line, varname, entry]
                    if os.path.isfile(var[0]) and var[1].isnumeric(): # Simple check
                        code_slices.append(run_slicer(
                                bc_file,
                                {
                                    "file": var[0],
                                    "line": var[1],
                                    "varname": var[2]
                                },
                                var[3],
                                forward
                            )
                        )

                elif len(var) == 5:
                    # with label
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
                                forward
                            )
                        )

                        labels.append(var[4])

        if len(labels) > 0 and len(code_slices) != len(labels):
            raise RuntimeError('Number of labels and code slices do not match.')
    
    else:
        cmd = ["llvm-src-slicer", "-sc", sc[0]]
        if entry is not None:
            cmd += ["-entry", entry[0]]
        if forward:
            cmd += ["--forward"]
        cmd += [bc_file]

        try:
            ret = subprocess.run(cmd, check=True, capture_output=True, text=True)
        except FileNotFoundError as err:
            print(err, file=sys.stderr)
            print('Please check the arguments and make sure llvm-src-slicer is in the PATH.', file=sys.stderr)
            exit(-1)

        if ret.returncode != 0:
            print(ret.stderr)
            raise RuntimeError('llvm-src-slicer does not exit normally.')

        code_slices.append(ret.stdout)

        
    if not _print:
        pkl_dict = {
            'code_slices': code_slices,
            'labels': labels
        }

        with open('sliced_data.pkl', 'wb') as pklfile:
            pickle.dump(pkl_dict, pklfile)
    else:
        print(code_slices[0])

if __name__ == "__main__":
    main()

    
        
