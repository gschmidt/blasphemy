/* Policies:
 *  It's up to you to not create watch cycles or weird shit
 *  It's single threaded
 *  Changes are applied immediately and nonatomically (maybe revisit)
 *   (but really this is a matter of private object policy ..)
 *  Have the same code running on both the client and server. Make the
 *   server-only parts noops on the client. Then, define stripping out
 *   the noops as being in the domain of the minifier.
 */

/* Goals:
 *  Nobody knows about the database (how the backend services get their
 *    data, and update it, is their own business)
 *  Clean Javascript interfaces -- no buying into special abstractions
 *    necessary unless you want their benefit (and they should be minimal)
 */

/* Questions:
 *  - How are watches GC'd?
 *  - Who can mutate? How does that work?
 *  - How do queries work?
 *    - Expressing grouping of objects into shards
 *    - Expressing associations
 *    - Stuff like list membership?
 *    - Cooperating with a datastore that can have columns added to it
 *  - How to decide what data to load on the server? (Maybe you just
 *    provide some big, chunky arrays as input into the render code?
 *    But that could duplicate a lot of data potentially, if it appears
 *    multiple times? "Tough, earn it" by writing server code to choke
 *    the data down to deduplicated form, and then expand it on client?
 *    .. not clear this is really a problem in real applications ..
 *  - Key point/example: how to specify attributes to createliveelement?
 *  - How to handle pointers to objects w/o this turning into an
 *    object database?
 *  - Saving client session state for hot code push? (Note, no need to
 *    ever push to server.. this is about hot updates) (though this is
 *    asking for trouble in JS, given how easy it is to get into the
 *    global namespace)
 *
 * Also:
 *  - How to handle security contexts?
 *  - How to handle client-side speculative mutator simulation?
 *
 * Possibly RunMutation(shard_id, func), where func is allowed to
 * call mutators (which are just obj.write(key, value) and normal-looking
 * list manipulation functions) but only those that affect shard_id?
 *
 * What if we kept all of the data in RAM? Well, getting the redo log
 * right could be a mess..
 *
 * Tempting to add a datastore abstraction to the client (that is thought
 * of as syncing data that is 'needed'? Where did that concept evaporate
 * to, anyway?) so that we can play snapshot/restore games on it for
 * local mutation simulation. But, does that require that we buy into
 * a side-effect-free function from datastore to DOM?
 */

/* Client-side simulation goes with the actual scheme of stubbing the
object by moving some code to the client. So really, this is private
to the object, and a function of however the object is getting its
data from the server. Basically, it's a form of remoting. So! The
side-effect-free-ness is confined to individual objects!!!

But, consider how this interacts with determining which data is sent
(perhaps poorly..) basically, is there a factoring into client and
server logic that satisfies both? */

// object in the sense of struct
ObservableObject = subclass(Object, {
    /**
     * Call func(x) when this.key_name changes to value x, and also
     * call it initially.
     *
     * For objects, this is only called when their eq?-ness changes,
     * that is, when they change to an actually different instance
     * of the object. This mean that for arrays, you'll want to register
     * a watch on the array.
     */
    watch : function(key_name, func) {
    } ,

    /**
     * Call func(key, value) when any attribute of this object changes,
     * and also call it initially.
     */
    watchAll : function(func) : {
    } ,

    /**
     * Change the value of a property and trigger observers, database
     * writes, whatever. Typically not remoted, but could be..
     */
    write : function(key_name, value) : {
    }
});

SomehowExtendArrayWith {
    /**
     * watcher funcs:
     *  - changed(offset, new_value)
     *  - inserted(offset, new_value)
     *  - deleted(offset)
     * probably easier to take these as args -- they'll be declared as
     * anonymous functions, in practice. but might still want to take them
     * as attributes on a { } since that will document which is which
     */
    watchArray : function(watcher) {
    }
});

/* Hmm, there is a common case of a FooManager that is just a database object that you can watch, with no particular state. */
    /* EXCEPT, that nobody can write to it except through calling mutators. */
UserManager = subclass(ObservableObject, {
} , {
    // observable list of topic refs
    subscriptions : [],
    addSubscription : function(x),
    removeSubscription : function(x)
});


/* Given a bunch of arrays, return their (watchable) concatenation. */
/* QUESTION: what happens if passed a static array (no watch method)? was it on purpose or an accident? */
DynamicConcatenate = function(a, b, c. ...) {
};

/* Return a DOM node -- handlers are registered on the returned node to
   update it as attributes/children change. children is just an array
   (watchable, if it is to be live.) Quite possibly each element in the
   array is another live DOM node. But there could also be a bunch of
   intervening non-live nodes, and then a live node deeper..

   'children' may well be the result of DynamicConcatenate

   XXX attributes is more of a mess (in terms of how you'd specify
   it.) also, consider derived values, dynamic_y = (dynamic_x*2)
*/
CreateLiveElement = function(attributes, children) {
});

// XXX provide a template system?

// XXX how to provide for global mode switching? hopefully not through
// watches in this way?