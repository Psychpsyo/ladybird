# Layout Architecture for Fragmentation

NOTE: Not all of this is merged or implemented into ladybird yet.
This is simply an overview of my current implementation plan/strategy so that it can be reviewed up-front.
For things that are implemented, I will link to the relevant code in my current PRs like this: [ref](https://github.com/LadybirdBrowser/ladybird/pull/8557/changes#diff-411c2a84372175ab6fb8393a63d3aaf7a2f057a897ac17a7ca5b18eaa41297d3R19)

I sometimes use terms like "height" or "Y position" to refer to sizes in the fragmentation direction or the progress
along a fragmented flow. In horizontally fragmented flows, these terms should be understood to refer to widths and X
positions instead. (and vice versa)

## Overview

Fragmented layout refers to laying out some flow of content into multiple, disconnected areas, called [fragmentainers](https://www.w3.org/TR/css-break-4/#fragmentainer).
Currently, this is required by [multi-column layout](https://developer.mozilla.org/en-US/docs/Web/CSS/Guides/Multicol_layout)
and when laying out for printing onto separate pages. In multicol, each column is a fragmentainer, whereas in print
mode, each page is a fragmentainer.

## Basic Constraints

This section serves to lay out a fairly complete, yet high-level overview of the problem that is fragmentation, calling
out various aspects that might be tricky to implement or need special considerations.

### Layout must be fragmentation-aware

Fragmentation needs to be considered during layout. Simply laying out the to-be-fragmented flow and then slicing it
into pieces does not work for multiple reasons:

1. We should avoid cutting images and text in half as far as possible. (content like this is called "[monolithic](https://www.w3.org/TR/css-break-4/#monolithic)")
2. Fragmentainers can vary in width, so certain layout considerations need to be recalculated for each
   fragmentainer, before putting further content into it. (like the available horizontal space)
3. The flexbox spec [recommends](https://www.w3.org/TR/css-flexbox-1/#pagination:~:text=When%20breaking%20a%20multi%2Dline%20column%20flex%20container)
   special ordering of items, should the whole flexbox span across a fragmentainer break in the flex direction.
4. All layout modes (block, table, flex and grid) have rules about where it is valid to fragment them and what that
   does to the surrounding layout.

### Box fragments must be drawn as such

Depending on the value of `box-decoration-break`, fragmented boxes need to be rendered as if the entire box got rendered
and then sliced into pieces. This can visually slice things like a border-radius in half.
(Although the other value of `box-decoration-break` requires us to draw each box fragment as if it were the whole
element, backgrounds and all.)

Fragments always need to be transformed individually.

### Some HTML elements must be rendered more than once

Any header rows in a table need to be repeated in every fragmentainer, so we will need to be able to lay them out and
draw them multiple times, potentially at different widths, in [varying-size fragmentainers](https://www.w3.org/TR/css-break-4/#varying-size-boxes).

### Parallel Flows

Layout within a fragmentation container may consist of multiple [parallel flows](https://www.w3.org/TR/css-break-4/#parallel-flows).
Examples for this are table or grid cells that sit next to one another, or floated boxes that sit next to the main flow
of content in a BFC (block-formatting context). These flows need to be broken independently of one-another.

### Fragmentation Direction

A fragmentation context only ever fragments in one direction, called the [fragmentation direction](https://www.w3.org/TR/css-break-4/#fragmentation-direction).
Although it is possible to nest fragmentation containers of varying directions, which could lead to a box that wants to
be fragmented in two directions in very unfortunate circumstances. We might want to sidestep this entirely (I think we
reasonably could) but my approach can also be expanded to support this two-axis fragmentation.

### Ideal Breakpoints

The spec defines several mechanisms which can be used to incentivize the browser to prefer certain breakpoints over
others. Examples of this are the CSS properties `widows`, `orphans`, `break-before`, `break-inside` and `break-after`,
as well as the inherent requirement not to slice monolithic content in half. (generally, as far as possible)

## Our Approach

Ladybird handles fragmentation by defining a FragmentationContext class, which represents a series of fragmentainers.
[ref](https://github.com/LadybirdBrowser/ladybird/pull/8557/changes#diff-411c2a84372175ab6fb8393a63d3aaf7a2f057a897ac17a7ca5b18eaa41297d3R19)
This fragmentation context gets passed along to the layout functions of all the formatting contexts which will then
coordinate their layout decisions with the fragmentation context to make sure that they observe and consider
fragmentainer breaks.

Different types of fragmentation contexts (paginated vs multicol) get their own subclasses of FragmentationContext to
account for the sometimes different breaking and content distribution considerations that they impose. [ref](https://github.com/LadybirdBrowser/ladybird/pull/8557/changes#diff-411c2a84372175ab6fb8393a63d3aaf7a2f057a897ac17a7ca5b18eaa41297d3R42)

### Layout

#### Basic Structure

UsedValues are calculated once per layout node, and augmented with a list of fragments that the node is to be broken
into. [ref](https://github.com/LadybirdBrowser/ladybird/pull/8771/changes#diff-3963a17ab5f49e4277de7430234bca2adf5dccd151cc969b1a10a2685d59c7c8R247-R253)
The reasoning for this is that everything but the position and size of the fragment in question is shared between all
fragments of this node and the box's original size, had it not been sliced, needs to be remembered as well. I suspect
that there will be some places that need to be adjusted to look at these fragments if necessary, but not too many.  
(One example would be BFC height calculation for BFCs with block-level children)

**NOTE:** I don't have strong opinions on whether or not the fragment list is optional. It could just as well be non-
optional and always contain at least one fragment. We will however need to still store the box's complete size on the
UsedValues, so that we can know how tall, relative to the full thing, a fragment is for purposes of background and
border painting.

There is two types of layout nodes that interact differently with fragmentation contexts. Monolithic content, that we
should not slice into fragments wherever possible and non-monolithic content that can more readily be sliced apart. We
assume that nodes that can contain further content are generally not monolithic. The places that these are sliced in,
as well as the number of fragmentainers they span, is primarily determined by what fragmentainers their monolithic
descendants end up in. The positioning of monolithic content on the other hand is more directly affected by
fragmentainer breaks, as detailed below. Note that this is not a hard-and-fast rule, but rather a general idea, and
formatting contexts may need to do deviate from it at times.

**NOTE:** Replaced elements with shadow trees (such as videos) present an exception to this. I think that for these
cases, we need to have an up-front check to determine if a box should be treated as monolithic, despite the fact that
it contains content that would, under normal circumstances, fragment happily. See "Pseudo-Monolithic Content" at the
end of this document for more info.

#### Process

Conceptually, we do the main part of our layout work as if all fragmentainers were glued end-to-end in the fragmentation
direction while considering any fragmentation breaks we come across in the process. Only when giving a box its final
position do we offset or slice it into the correct physical location of the fragmentainer(s) that it needs to go into.

This happens as follows:
Whenever a formatting context wants to lay out a monolithic piece of content, it consults the fragmentation context and
asks it what to do with that piece of content. The three possible options here are a) to place that piece of content
where it is, b) to shunt it downwards into the next fragmentainer or c) to fragment it into multiple pieces.

**NOTE:** We may want to consider a Shunt&Fragment option to minimize the number of breaks in a piece of monolithic
content that needs to be fragmented.

Based on the response, the formatting context puts the box in the right place, potentially consulting the fragmentation
context again to figure out by exactly how far to shunt it or how many fragments of what height to break it into. The
formatting context also adjusts its current Y position so that subsequent content will be laid out correctly as well.
[ref](https://github.com/LadybirdBrowser/ladybird/pull/8785/changes/cb3903290e391ef5021e8bff1492b7fc26ffc0e6#diff-220d815bce7a0632c20a2a182c70dd9064ff948827a1cf7fad94fe106339130cR273-R288)

**NOTE:** This specific interaction should be formalized out a little more, as it gets to serve as a clear transition
point between fragmentainers that can also kick off recalculations of the available width and things that depend on it.
(such as the column-count, in case of a multicol context that is nested into and broken by some other fragmentation
context.)

**NOTE:** Fragmentainer overflow can still happen. We do not slice/position purely based on Y position, we do so based
on a 'fragmentation decision' made for a piece of monolithic content. If such content is placed so that it overflows the
fragmentainer, the formatting context's current Y position is adjusted back up a little, where it will now start laying
out the next piece of content, as that is where the start of the next fragmentainer is. This means the fragmentainer
that a fragment ends up in is generally the fragmentainer that its top edge falls into, although this is largely
coincidental an can be extended with more intricate logic in the future, if we ever feel the need to support overflow
where a piece of monolithic content is off the bottom edge of the fragmentainer in its entirety.

Now, depending on the breaking restrictions in place here, it might seem necessary to also shunt some amount of prior
content into the next fragmentainer to honor certain combinations of `widows`, `orphans`, `break-before` and
`break-after`. Currently, there is no concrete implementation plan for this. Chromium, when it gets to this point,
looks back at all the breaking opportunities that it did not take, and then possibly [rewinds and re-runs layout](https://developer.chrome.com/docs/chromium/renderingng-fragmentation#:~:text=we%20need%20to%20return%20and%20re%2Drun%20layout)
with the knowledge that it should take one of those instead. We can consider a similar solution when we get to that
point in the implementation.

**NOTE:** There will need to be some extra considerations here for varying-width fragmentainers, especially ones with
multiple parallel flows, but that sounds like a problem that can be figured out when we get to it as well.

Next, when a formatting context reaches the end of a non-monolithic box, like regular block-boxes that just had their
contents laid out, it might need to fragment this box so that it still wraps its contents. (instead of just spilling
out the bottom of whatever fragmentainer it started in.) To do this, the fragmentation context is consulted to figure
out the required number of fragments and their positions, based on the size and current offset into the surrounding
fragmentainers. [ref](https://github.com/LadybirdBrowser/ladybird/pull/8771/changes/357317d332a5e527f0bcd51ffe51ff01cf48f5d7#diff-d030c5b49d2330ac5d8062dccb2f1cca24d384d59223e67f23921b5844dcf893R1014-R1034)

I also just noticed that the linked box fragmentation code here will misbehave if a box has children that overflow a
fragmentainer, which it is expected to wrap, even beyond the end of the fragmentainer. I can think of multiple ways to
address this:
1. Track a box's required height per-fragmentainer while laying out its children, making note of any fragmentainer
overflow that it will need to wrap. We could introduce an explicit Fragmentainer struct for this, which can keep track
of the amount of overflow it experienced per parallel flow that went into it.
2. Keep track of all the parent boxes we are currently doing layout inside of and break during this layout, rather than
after positioning all the children.

Number 2 is more similar to [what Chromium does](https://developer.chrome.com/docs/chromium/renderingng-fragmentation#:~:text=the%20layout%20of%20descendants%20has%20to%20stop%20at%20a%20break).

### Painting

In unfragmented flows, a paintable will be generated from the entirety of a UsedValues, like normal. But if it got
fragmented during fragmented flow, a paintable will be created per fragment and receive extra information, telling it
which fragment it is supposed to be. This is currently indicated by the fragmentation direction, an offset from the
start of the node, and the size in the fragmentation direction that the whole, unfragmented node had. [ref](https://github.com/LadybirdBrowser/ladybird/pull/8803/changes/f678fa9e2515f5e40cdd9d8c06495dc24c524b61#diff-1e6c5c60dcca1baff076e29a15b74d1e9e86660360d8a3731d24eea925fe7732R302)

**NOTE:** I once again have no strong feelings either way regarding whether not we should consider fragment data as
something optional that gets added for fragmented paintables only, or if even unfragmented paintables should be told
that they are a fragment that is the size of the whole thing. That said, I do find the "optional extra fragment data"
to be more in-line with my mental model of fragmentation as a thing that optionally applies only sometimes. (namely,
when a node is cut apart into multiple fragments.)

Depending on the value of `box-decoration-break`, the paintable will use this information to either draw a full box
around just the fragment itself, or to draw a box at the original size, which will then have the parts that do not
belong to this fragment clipped away. (This currently happens during display list building, since that is where we
apply all other forms of clipping information. [ref](https://github.com/LadybirdBrowser/ladybird/pull/8803/changes/f678fa9e2515f5e40cdd9d8c06495dc24c524b61#diff-8e1695213d0600acf51914ef9ec7a48b0e347ba6a68a94729b02435469f144ddR305-R324))

**NOTE:** Drawing-then-clipping the full box seems like the cleanest way to do this, because there is cases where a
`border-radius` will need to be visually cut in half by this process. Calculating how to render that otherwise seems
like undue complication for no real benefit. Even though it might be somewhat necessary when we compute the shape of
the box's outline, which is expected to visually wrap the shape that we get by unioning all the fragments together.
Then again, all the other browsers struggle with this as well and switching between these options would not incur huge
rewrites either way.

### Multicol Balancing and Column Height

In multicol layouts, it is sometimes required for the browser to distribute the content in all columns such that they
all have a similar amount of content. Meaning that we should minimize the overall height deviations between columns.

We do this in two passes:
1. Lay the content out into one long column with no height limit. This, divided by the column count, gives us the ideal
height for each column. In a perfect world, we manage to make all columns this exact height. [ref](https://github.com/LadybirdBrowser/ladybird/pull/8785/changes/cb3903290e391ef5021e8bff1492b7fc26ffc0e6#diff-d030c5b49d2330ac5d8062dccb2f1cca24d384d59223e67f23921b5844dcf893R128)
2. Flow the content into the actual number of columns. During this, the column fragmentation context will make
fragmentation decisions that are optimized to give us as-equal-as-possible column heights. [ref](https://github.com/LadybirdBrowser/ladybird/pull/8785/changes/cb3903290e391ef5021e8bff1492b7fc26ffc0e6#diff-cf706db33be76d271c08fa8953d8bdf4173b4c71747778318e867a4a77501deeR48)

**NOTE:** We might want to apply some additional heuristics in step 1 to account for cloned table headers.

The final height for multicol containers is determined the exact same way it is for regular BFCs, so if we place content
past the ideal height, the container's height as a whole will include that extra content. One might assume that this
makes fragmentainer overflow impossible, but that is a non-issue, as overflow can (in theory) only happen in height-
constrained multicol containers. Although the current implementation prefers fragmenting in those cases anyways. [ref](https://github.com/LadybirdBrowser/ladybird/pull/8785/changes/cb3903290e391ef5021e8bff1492b7fc26ffc0e6#diff-cf706db33be76d271c08fa8953d8bdf4173b4c71747778318e867a4a77501deeR56)
(See https://github.com/w3c/csswg-drafts/issues/13781 for my reasoning.)

#### Height-Balancing Fragmentation Decisions

When a piece of content is to be laid out in a balanced multicol container, the column fragmentation context will decide
whether to place or shunt it by considering what that would do to our balancing.

If the content exceeds the ideal height, placing it would make the column in question too tall, but shunting it would
make the column to small. We take the option that introduces the least amount of height variation. This leaves us with
a new problem:

If we shunted the content down, we have introduced a gap that was not accounted for when we calculated our ideal height.
This means that if we keep going like this, we will reach the end of the last column's ideal height with a bunch of
left-over content that did not make it into those gaps. Now we'd have no other option than to place it anyways, which
would make the last column way too tall. Conversely, if we placed the content and allowed it to grow that column
up-front, we are now in the oppsite situation. We have placed some content outside of our ideal-height rectangles which
means we won't be able to fill them all up as planned. We will reach the end of our content before we reach the end of
the last column's ideal height. If we keep going like this, the last column won't be as tall as we'd like it to be.

To combat this, we don't base our fragmentation decisions purely on the current column's height. Instead, we keep track
of by how much we are currently over- or under-shooting the ideal end of the last column. If we introduced a gap, we
have too much content left. If we went over the ideal height, too little. We refer to these states as having a content
deficit, or a content surplus. [ref](https://github.com/LadybirdBrowser/ladybird/pull/8785/changes/cb3903290e391ef5021e8bff1492b7fc26ffc0e6#diff-411c2a84372175ab6fb8393a63d3aaf7a2f057a897ac17a7ca5b18eaa41297d3R79-R89)

So when we need to decide whether or not to place content, we calculate how much additional deficit or surplus either
decision would incur and then take the one that, combined with our current deficit/surplus, brings us closer to breaking
even. [ref](https://github.com/LadybirdBrowser/ladybird/pull/8785/changes/cb3903290e391ef5021e8bff1492b7fc26ffc0e6#diff-cf706db33be76d271c08fa8953d8bdf4173b4c71747778318e867a4a77501deeR104-R109)

### Miscellaneous

#### Cloned Table Headers

For cloned table headers, the table formatting context should remember them, and, whenever it gets confronted with a
`Shunt` or a `Fragment` fragmentation decision, it should lay them out again. It might make sense to have multiple
UsedValues here, as there is actually multiple separate instances of the element floating around that can be
fundamentally different from one another due to being in different fragmentainers of potentially varying widths.

#### Pseudo-Monolithic Content

Some content, like video elements, should be treated as monolithic, despite that fact that it can contain a shadow-tree
that could conceivably be laid out with regards to fragmentation like anything else. I propose that when such a layout
node is about to be laid out, we check that it is monolithic and request a fragmentation decision from the fragmentation
context. If we just need to place or shunt, we do so and lay the box out like normal. If we need to fragment, we pass a
new type of FragmentationContext to said box that simply has unnegotiable breaks in the right spots, so that the layout
of the boxes children will fragment all of them in the correct positions.
