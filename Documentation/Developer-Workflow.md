# Developer Workflow

## Introduction

This documents our typical workflow. At a high level this covers how we
expect issues to work, what our development cadence is, and how we do design
for larger features.  And by we, we mean the whole community. These guidelines
hopefully foster collaboration and high quality, useful software.
(Some fun wouldn't hurt either.)

## Issues

We use GitHub issues to track work, so if you're working on something, open
an issue.  It gives the community a place to have a conversation about the
fix/feature that's being worked on.  In addition it helps everybody keep
track of who is working on what so that we can minimize duplication and
redundant work.

The typical issue flow is as follows:
- Have a great idea, or find a bug, and open issue.
- Issue is discussed on GitHub and a direction is established.
- Somebody signs up to do the work and potentially a sprint milestone is
  assigned if the work directly effects a particular near term project
  [goal](https://github.com/dotnet/llilc/blob/master/Documentation/llilc-milestones.md).
- Ongoing notes on the implementation are added to the issue as work progresses.
- Work is completed, tested, PR'd, and merged in.
- Issue is closed

Note on 'assigning' issues: The only people that can be assigned to an issue
in GitHub are people that are in one of the project groups (either *llilc* -
read/write access, or *llilc-community* - read only access).  We add people
to the Community group after their first PR is accepted by the collaborators.
This enables you to be added as the assignee for issues.  So if this is your
first issue, note your Github account name in the issue as the person
working on it and others will understand that it's in flight.

Note on read/write access: We'd like to develop a broad set of committers to
the project from the community (we don't have any non-MS committers at this
point) we think that this should happen after people build a track record in
the llilc-community group.  The expectation is that after few (3-5) non-trivial
contributions a community member can request to be added to the project group
and gain read/write access to the source.  With this access also comes the
expectation that the collaborator will help review others PR's, help maintain
documentation, as well as generally facilitate the smooth running of the project.

## Workflow

We use 3 week sprints in a agile(-ish) model.  These sprints are added as
Milestones in Github (not to be confused with the project [Milestones](llilc-milestones.md)
which are larger granularity proof points of functionality.)  Look at the
[Github milestones page](https://github.com/dotnet/llilc/milestones) to see
the individual sprints and their end dates. These sprints are used by core
members of the team to managed work toward near term goals, but it is not
necessary for community contributors to use these milestones.  The only place
where this could potentially cause some complication is if a particular
feature/fix is needed for a near term goal.  In this case we would have a
conversation about assigning the issue to a milestone and potentially moving
the work to a collaborator that can commit to a solution in the needed time
frame. We're attempting to have a balance between an open and friendly
environment to hack and the necessity to drive functionality (as well as
  meet product goals).

A typical flow is as follows:
- Day one of a sprint milestone work is self-assigned by developers based on
  a combination of priority and what the think that they can get done in a 3
  week period.
- Through out the sprint notes are added to the issues assigned to the sprint
  outlining progress.
- Issues are closed as they are merged back in and the CI system stays green.
- On the first day of the next sprint, any items not completed are triaged
  and moved to the appropriate sprint. (Typically this is the next sprint)

## Design process

As stated above, all features start as issues.  If as part of the conversation
about an issue, the community decides that a particular feature is large or
complicated enough to warrant a separate write up a document will be created
by interested parties describing the design and the staging of bring up.
This document will be reviewed via PR and checked in as separate markdown in
the project Documentation directory off the root.  A good example of this is
the [EH design document](llilc-jit-eh.md). As you can see today only larger,
more complicated areas warrant this treatment.  I want to highlight that
typical work will be handled via issues, reserving this process for areas
that require months of development to bring to fruition.
