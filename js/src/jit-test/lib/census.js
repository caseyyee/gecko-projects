// Functions for checking results returned by Debugger.Memory.prototype.takeCensus.

const Census = {};

(function () {

  // Census.walkCensus(subject, name, walker)
  //
  // Use |walker| to check |subject|, a census object of the sort returned by
  // Debugger.Memory.prototype.takeCensus: a tree of objects with integers at the
  // leaves. Use |name| as the name for |subject| in diagnostic messages. Return
  // the number of leaves of |subject| we visited.
  //
  // A walker is an object with three methods:
  //
  // - enter(prop): Return the walker we should use to check the property of the
  //   subject census named |prop|. This is for recursing into the subobjects of
  //   the subject.
  //
  // - done(): Called after we have called 'enter' on every property of the
  //   subject.
  //
  // - check(value): Check |value|, a leaf in the subject.
  //
  // Walker methods are expected to simply throw if a node we visit doesn't look
  // right.
  Census.walkCensus = (subject, name, walker) => walk(subject, name, walker, 0);
  function walk(subject, name, walker, count) {
    if (typeof subject === 'object') {
      print(name);
      for (let prop in subject) {
        count = walk(subject[prop],
                     name + "[" + uneval(prop) + "]",
                     walker.enter(prop),
                     count);
      }
      walker.done();
    } else {
      print(name + " = " + uneval(subject));
      walker.check(subject);
      count++;
    }

    return count;
  }

  // A walker that doesn't check anything.
  Census.walkAnything = {
    enter: () => Census.walkAnything,
    done: () => undefined,
    check: () => undefined
  };

  // A walker that requires all leaves to be zeros.
  Census.assertAllZeros = {
    enter: () => Census.assertAllZeros,
    done: () => undefined,
    check: elt => assertEq(elt, 0)
  };

  function expectedObject() {
    throw "Census mismatch: subject has leaf where basis has nested object";
  }

  function expectedLeaf() {
    throw "Census mismatch: subject has nested object where basis has leaf";
  }

  // Return a function that, given a 'basis' census, returns a census walker that
  // compares the subject census against the basis. The returned walker calls the
  // given |compare|, |missing|, and |extra| functions as follows:
  //
  // - compare(subjectLeaf, basisLeaf): Check a leaf of the subject against the
  //   corresponding leaf of the basis.
  //
  // - missing(prop, value): Called when the subject is missing a property named
  //   |prop| which is present in the basis with value |value|.
  //
  // - extra(prop): Called when the subject has a property named |prop|, but the
  //   basis has no such property. This should return a walker that can check
  //   the subject's value.
  function makeBasisChecker({compare, missing, extra}) {
    return function makeWalker(basis) {
      if (typeof basis === 'object') {
        var unvisited = Set(Object.getOwnPropertyNames(basis));
        return {
          enter: prop => {
            unvisited.delete(prop);
            if (prop in basis) {
              return makeWalker(basis[prop]);
            } else {
              return extra(prop);
            }
          },

          done: () => unvisited.forEach(prop => missing(prop, basis[prop])),
          check: expectedObject
        };
      } else {
        return {
          enter: expectedLeaf,
          done: expectedLeaf,
          check: elt => compare(elt, basis)
        };
      }
    };
  }

  function missingProp() {
    throw "Census mismatch: subject lacks property present in basis";
  }

  function extraProp() {
    throw "Census mismatch: subject has property not present in basis";
  }

  // Return a walker that checks that the subject census has counts all equal to
  // |basis|.
  Census.assertAllEqual = makeBasisChecker({
    compare: assertEq,
    missing: missingProp,
    extra: extraProp
  });

  // Return a walker that checks that the subject census has at least as many
  // items of each category as |basis|.
  Census.assertAllNotLessThan = makeBasisChecker({
    compare: (subject, basis) => assertEq(subject >= basis, true),
    missing: missingProp,
    extra: () => Census.walkAnything
  });

})();
