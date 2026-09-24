"""Compiler subprocesses with literal filenames and UTF-8 response files."""
import os
import shutil
import subprocess
import tempfile


def run(command):
    command = list(command)
    stage, response, output = None, None, None
    try:
        # Our objcopy invocations modify the last argument in place.
        inplace = os.path.splitext(os.path.basename(command[0]))[0].endswith("objcopy")
        index = command.index("-o") + 1 if "-o" in command else len(command) - 1 if inplace else None
        if index is not None:
            output = command[index]
            if "%" in output:
                # LLVM treats every % in its output path as a random character
                # in its temporary-file template, including directory names.
                # A relative staging name keeps even a % in cwd out of that
                # template. Python moves the finished file to the literal path.
                stage = os.path.relpath(tempfile.mkdtemp(prefix="memories-link-", dir="."))
                command[index] = os.path.join(stage, "output" + os.path.splitext(output)[1].replace("%", "_"))
                if inplace:
                    shutil.copy2(output, command[index])
        staged_output = command[index] if stage else None
        if sum(len(word) + 1 for word in command) > 30000:
            # Windows limits command lines to 32 K. The supported LLVM/GNU
            # tools accept UTF-8 @files on both hosts, regardless of locale.
            with tempfile.NamedTemporaryFile("w", encoding="utf-8", newline="\n", dir=".", suffix=".rsp", delete=False) as handle:
                handle.writelines('"%s"\n' % word.replace("\\", "\\\\").replace('"', '\\"') for word in command[1:])
                response = handle.name
            command = [command[0], "@" + response]
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
        if stage and not result.returncode:
            shutil.move(staged_output, output)
        return result
    finally:
        if response:
            os.remove(response)
        if stage:
            shutil.rmtree(stage)
