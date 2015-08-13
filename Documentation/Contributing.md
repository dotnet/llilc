# Contribution Guide

## Workflow
We follow the standard [GitHub workflow](https://guides.github.com/introduction/flow/).
We like to track ongoing work using GitHub issues as described in the
[developer workflow document](Developer-Workflow.md).
 - To submit changes, please create a personal fork of the LLILC repo, push
   your changes there, and issue a pull request to merge those changes into
   the master branch of the main repository.
 - Please be sure to perform the appropriate [formatting](#coding-conventions-and-formatting)
   and [testing](#testing) as described below before submitting your pull
   request.
 - Whenever possible, please divide any changes you want to make into a
   series of small, incremental changes that can be submitted and reviewed
   independently.  Doing so will
    - Make the code easier to review (allowing for higher quality review
      feedback)
    - Keep your work aligned with LLILC's direction and architecture along
      the way (avoiding wasted time pursuing a direction that ultimately
      gets abandoned)
    - Spare you the pain of keeping large outstanding changes up-to-date
      with tip-of-tree
 - Occasionally we'll deviate from this model and use long-lived branches,
   which follow [another workflow](Long-Running-Branch-Workflow.md).

## Coding Conventions and Formatting
Any code being submitted must be formatted per the instructions in the
[code formatting document](Code-Formatting.md).  We follow LLVM's coding
conventions, as described in our [coding conventions document](llilc-Coding-Conventions-and-Commenting-Style.md).

The clang-format and clang-tidy exes used in the lab are generated nightly
and can be downloaded for local use: 
[here for clang-format](http://dotnet-ci.cloudapp.net/view/dotnet_llilc/job/dotnet_llilc_code_formatter_drop/lastSuccessfulBuild/Azure/processDownloadRequest/build/Release/bin/clang-format.exe)
and [here for clang-tidy](http://dotnet-ci.cloudapp.net/view/dotnet_llilc/job/dotnet_llilc_code_formatter_drop/lastSuccessfulBuild/Azure/processDownloadRequest/build/Release/bin/clang-tidy.exe).

## Testing
The [test harness document](Testing.md) describes the tests we expect to be
run for each submission and how to run them.

## Open Areas
Looking for an area to contribute to?  See our [list of open areas](Areas-To-Contribute.md).
