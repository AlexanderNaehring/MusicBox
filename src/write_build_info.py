Import("env")
import subprocess, os
try:
    g = subprocess.check_output(["git","rev-parse","--short","HEAD"]).strip().decode()
except:
    g = "unknown"
with open(os.path.join(env['PROJECT_DIR'], "src", "git_info.h"), "w") as f:
    f.write('#define GIT_COMMIT "%s"\n' % g)