# Long-Running Branches

The first rule of long-running branches on the main LLILC repo is that they
should be the exception, not the norm.  Most changes should be pushed to a
personal fork and a pull request issued to merge from the fork directly to
the master branch of the main LLILC repo, per the [contribution guide](Contributing.md#workflow).

Occasionally, however, we'll need to make changes that simultaneously
 - Involve a lot of code
 - Will take a long time to develop
 - Will begin with tentative/exploratory changes that are likely to be revised
   before the feature is complete
 - Are likely to destabilize the codebase until complete
 - Don't have a reasonably efficient incremental implementation path available

For such changes, it makes sense to work in a long-lived branch, then
retroactively parcel out the change into non-destabilizing pieces, as
incremental as possible, for integration back into the master branch.  When
key features meet these criteria, we may create a long-lived branch on the
main repo where the work will occur, to publicize it and facilitate
collaboration/review in even the early tentative stages.  For example, the
initial bring-up of [exception handling support](https://github.com/dotnet/llilc/tree/EH)
and [precise garbage collection support](https://github.com/dotnet/llilc/tree/GC)
are both following this model.

The expected workflow for long-lived branches in the main repo is:
 1. The branch maintainer(s) will regularly push merges from master to the
    long-lived branch
    - Merging is preferred over rebasing to simplify collaboration
    - Pull requests are not expected for these merges (there's not really
      anything to review); the branch maintainer(s) will push them directly
 2. All other changes in the branch should be made via pull requests from
    personal forks.  These changes should be small and incremental, but may
    be destabilizing (when warranted to make forward progress).  Getting
    code reviews along the way should help us end up with higher quality code
    and fewer late-stage surprises than delaying review until merging back to
    master would.
 3. Any such branch may have its own policy for what level of testing is
    expected/required to maintain the quality bar as changes are made.
    Code style/formatting should be held to [the usual standards](Code-Formatting.md).
 4. When the feature is stable/complete, the branch maintainer(s) should
    rebase/repackage the changes in a way that is logical to merge into master.
    - The individual changes to be merged must be stable, and should be as
      incremental as possible.
    - Pull requests should be opened to merge the changes into master.
    - Typically, each incremental change should have its own pull request.
    - A single pull request for a series of incremental changes is ok if
      they are a straightforward replay of already-reviewed commits from the
      branch.
