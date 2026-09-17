import QtQuick

// A named group of bar widgets — where a widget goes, and the whole of what that widget knows about it.
//
// The bar places a few of these (a left one and a right one today, and a centre one when something belongs
// there), and a widget is placed by being declared inside the group it belongs to. Nothing a widget declares
// carries a coordinate, an anchor or an offset of its own: the group is the only thing that knows what is
// where, so the answer to "where does the next widget go" is a line inside one of these groups rather than
// a search for room among the anchors.
//
// Two things the group owns, and they are why this is a component rather than a bare `Row`:
//
//   * **The height convention.** Every widget of the group is given the group's height, and a widget draws
//     what it has to draw in the middle of the height it was given. So a widget never has to know how tall
//     its neighbour is, and widgets of different natural sizes line up along one line — which is the thing a
//     hand-anchored bar gets wrong the first time somebody adds a widget with a different font size. It is
//     enforced below rather than described here, because a widget that chose its own height is exactly the
//     widget that would break the row, and a convention nothing checks is a comment.
//   * **The name.** The bar names each group — "left", "right" — and that name is also the object's name in
//     Qt's own tree, which is how anything outside the component addresses a group without counting the
//     bar's children to work out which one it has hold of.
//
// One thing this does not do, written down rather than left to be discovered. A group cannot push another
// one aside: a centre group is centred on the bar, so a bar too narrow for its side groups would overlap
// them rather than shrink them. That is a decision for the group that has to give way, and it belongs with
// the widget that first needs the room. And a widget that is not visible is not laid out and holds no space,
// which is what a positioner does with an invisible child — a widget's honest empty state stops reserving
// room the moment it appears.
Row {
    id: group

    // What the bar calls this group, and, through `objectName`, what Qt's object tree calls it.
    property string name
    objectName: name

    // The gap between two widgets of this group, and the only spacing a group has.
    spacing: 6

    // The height convention, enforced. These are the two moments a widget can be the wrong size — the group
    // gains or loses a widget, or the group's own height changes — and the guard is what keeps the group
    // from rewriting a height that already matches, so it settles instead of waking itself up again.
    Component.onCompleted: fit()
    onChildrenChanged: fit()
    onHeightChanged: fit()

    function fit() {
        for (let i = 0; i < children.length; ++i) {
            const widget = children[i]
            if (widget.height !== height)
                widget.height = height
        }
    }
}
