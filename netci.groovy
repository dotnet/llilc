import jobs.generation.Utilities;

def project = GithubProject
def branch = GithubBranchName

static void addMultiScm(def myJob, boolean isPR) {
  if (isPR) {
    addPRTestMultiScm(myJob)
  }
  else {
    addMultiScm(myJob)
  }
}

static void addPRTestMultiScm(def job) {
  job.with {
    multiscm {
      git {
        remote {
          github('Microsoft/llvm')
        }
        branch('*/MS')
        wipeOutWorkspace(true)
        shallowClone(true)
        relativeTargetDir('LLVM')
      }
      git {
        remote {
          github('dotnet/coreclr')
        }
        branch('*/master')
        relativeTargetDir('coreclr')
      }
      git {
        remote {
          github('dotnet/llilc')
          refspec('${GitRefSpec}')
          url('${GitRepoUrl}')
        }
        branch('${sha1}')
        relativeTargetDir('LLVM\\tools\\llilc')
      }
    }
  }
}

static void addMultiScm(def myJob) {
  myJob.with {
    multiscm {
      git {
        remote {
          github('Microsoft/llvm')
        }
        branch('*/MS')
        wipeOutWorkspace(true)
        shallowClone(true)
        relativeTargetDir('LLVM')
      }
      git {
        remote {
          github('dotnet/coreclr')
        }
        branch('*/master')
        relativeTargetDir('coreclr')
      }
      git {
        remote {
          github('dotnet/llilc')
        }
        branch('*/master')
        relativeTargetDir('LLVM\\tools\\llilc')
      }
    }
  }
}

[true, false].each { isPR ->
  ['Debug', 'Release'].each { configuration ->
    lowerConfiguration = configuration.toLowerCase()
    def newJobName = Utilities.getFullJobName(project, "windows_nt_${lowerConfiguration}", isPR)
    
    def newJob = job (newJobName) {
      steps {
        batchFile("""cd llvm
echo |set /p="LLVMCommit=" > %WORKSPACE%\\commits.txt
git rev-parse "refs/remotes/origin/MS^{commit}" >> %WORKSPACE%\\commits.txt

cd tools\\llilc
echo |set /p="LLILCCommit=" >> %WORKSPACE%\\commits.txt
git rev-parse "refs/remotes/origin/master^{commit}" >> %WORKSPACE%\\commits.txt

cd %workspace%\\coreclr
echo |set /p="CoreCLRCommit=" >> %WORKSPACE%\\commits.txt
git rev-parse "refs/remotes/origin/master^{commit}" >> %WORKSPACE%\\commits.txt""")
        batchFile("""if exist build/ rd /s /q build
mkdir build
if exist test/ rd /s /q test
mkdir test

cd coreclr
./build.cmd ${lowerConfiguration}""")
        batchFile("""cd build
cmake -G \"Visual Studio 14 2015 Win64\" -DWITH_CORECLR=%WORKSPACE%\\coreclr\\bin\\Product\\Windows_NT.x64.Release -DLLVM_OPTIMIZED_TABLEGEN=ON ..\\llvm
\"%VS140COMNTOOLS%\\..\\..\\VC\\vcvarsall.bat\" x86 && msbuild llvm.sln /p:Configuration=${configuration} /p:Platform=x64 /t:ALL_BUILD /m""")
        batchFile("""copy %WORKSPACE%\\build\\bin\\${configuration}\\llilcjit.dll coreclr\\bin\\Product\\Windows_NT.x64.${configuration}
set COMPLUS_AltJit=*
set COMPLUS_AltJitNgen=*
set COMPLUS_AltJitName=LLILCJit.dll
set COMPLUS_GCCONSERVATIVE=1
set COMPLUS_ZapDisable=1
cd coreclr\\bin\\Product\\Windows_NT.x64.${configuration}
del mscorlib.ni.dll
crossgen.exe mscorlib.dll

if %errorlevel% neq 0 (
exit -1
)

if not exist mscorlib.ni.dll (
exit -1
)
del llilcjit.dll""")
        batchFile("""if exist target/ rd /s /q target
mkdir target

cd coreclr\\tests

C:\\Python27\\python %WORKSPACE%\\llvm\\tools\\llilc\\test\\llilc_runtest.py -a x64 -b ${lowerConfiguration} --ngen -d summary -r ..\\..\\target -j %WORKSPACE%\\build\\bin\\${configuration}\\llilcjit.dll -c %WORKSPACE%\\coreclr\\bin\\Product\\Windows_NT.x64.${configuration} || (echo There were failures with the new JIT& exit /b -1)

C:\\Python27\\python %WORKSPACE%\\llvm\\tools\\llilc\\test\\llilc_checkpass.py -d ..\\..\\target || (echo There were faulures.& exit /b -1)""")
      }
    }

    Utilities.setMachineAffinity(newJob, 'Windows_NT', 'latest-or-auto')
    Utilities.addStandardParameters(newJob, project, isPR, '*/master')
    addMultiScm(newJob, isPR)
    Utilities.addStandardOptions(newJob, isPR)
    if (isPR) {
      Utilities.addGithubPRTriggerForBranch(newJob, branch, "Windows ${lowerConfiguration}")
    }
    else {
      Utilities.addGithubPushTrigger(newJob)
    }
  }
}

[true, false].each { isPR ->
  ['Debug', 'Release'].each { configuration ->
    String lowerConfiguration = configuration.toLowerCase()
    def newJobName = Utilities.getFullJobName(project, "ubuntu_${lowerConfiguration}", isPR)

    def newJob = job (newJobName) {
      steps {
        shell("if which clang-3.5; then \n    export CC=\$(which clang-3.5)\n    export CXX=\$(which clang++-3.5)\nelif which clang; then\n    export CC=\$(which clang)\n    export CXX=\$(which clang++)\nelse\n    echo Could not find clang or clang-3.5\n    exit 1\nfi\n\n(cd coreclr && ./build.sh skipmscorlib ${lowerConfiguration} && cd ..) && (cd llvm && mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=${configuration} -DWITH_CORECLR=../../coreclr/bin/Product/Linux.x64.${configuration} .. && make -j 5)")
      }
    }

    Utilities.setMachineAffinity(newJob, 'Ubuntu', 'latest-or-auto')
    Utilities.addStandardParameters(newJob, project, isPR, '*/master')
    addMultiScm(newJob, isPR)
    Utilities.addStandardOptions(newJob, isPR)
    if (isPR) {
      Utilities.addGithubPRTriggerForBranch(newJob, branch, "ubuntu ${lowerConfiguration}")
    }
    else {
      Utilities.addGithubPushTrigger(newJob)
    }
  }
}

