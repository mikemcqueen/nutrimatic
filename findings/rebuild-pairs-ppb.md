i'm exploring how to integrate 'ppb' search terms into review-pairs.py.

first step is to explore how -u LETTERS propagate to nested steps.

imagine i want to end up with the following hierarchy:

pp3-dict-abc/
  pairs
  pp-1sq-5k/
    pairs
    ppb-pb3/
      pairs
    ppb-pb3-def/
      pairs
      
for ppb-pb3 the desire is that:

  1. ppb-pb3 is interpreted as --ss ppb,pb,pb,pb --pp FILE
  2. FILE is pp3-dict-abc/pp-1sq-5k/pairs
  3. the pairs in FILE were generated from the letter bag with the letters 'abc' removed
     due to those letters having been removed from the bag in the root pp3-dict-abc. This
     is as designed and is would work correctly.
     
for ppb-pb3-def the desire is:

  1. ppb-pb3-def is interpreted as --ss ppb,pb,pb,pb --pp FILE -u abcdef
  2. Note that -u is the concatenation of this steps LETTERS, and the root step's LETTERS.
     (In the general case, it would, I suppose, be the concatention of all ancestor step's
     letters). This is not specific to ppb, but should apply to all pbN style steps.  I
     don't know if this feature exists currently; I suspect it does not.
  2. FILE *could in theory* be pp3-dict-abc/pp-1sq-5k/pairs. The problem with that approach
     is that those pairs were the top-ranked results generated using only -u abc. With the
     addition of -u def, some of those pairs may be excluded. What we really want as input 
     to --pp is the top-ranked pairs generated using -u abcdef, it seems?
     
so, it seems that implies the actual structure we need is instead something like:
 
pp3-dict-abc/
  pairs
  pp-1sq-5k-def/
    pairs
    ppb-pb3/
      pairs
 
which implies:

  1. the nested ppb-pb3 is interpreted as --ss ppb,pb,pb,pb --pp FILE -u abcdef
  2. FILE is pp3-dict-abc/pp-1sq-5k-def/pairs
  3. *somehow* the pairs in that file are restricted to pairs which can be formed using
     the sentence LETTERS - abcdef.  this is the main point i'm confused about.  I suppose
     we could modify pair-prior to do this, but it's not clear to me if that's enough, or
     if we actually need to/should run a pp3-dict-abcdef in order to generate the correct
     pairs, and then have a child pp-1sq-5k cut of that?
  4. nested ppb steps with -u LETTERS probably cannot supported *directly* as directory names.
     rather, they must inherit a nested -u LETTERS from either parent "cut", or this must all
     exist in a separate pp3-dict-abcdef/ root hierarchy; because this is the only way to ensure
     that the --pp input properly reprents the top ranked pairs with the desired -u LETTERS.
  
Please provide feedback on whether you think this is correct and any issues you see and any
thoughts you have.
