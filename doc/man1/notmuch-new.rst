===========
notmuch-new
===========

SYNOPSIS
========

**notmuch** **new** [options]

DESCRIPTION
===========

Find and import any new messages to the database.

The **new** command scans all sub-directories of the database,
performing full-text indexing on new messages that are found. Each new
message will automatically be tagged with both the **inbox** and
**unread** tags.

You should run **notmuch new** once after first running **notmuch
setup** to create the initial database. The first run may take a long
time if you have a significant amount of mail (several hundred thousand
messages or more). Subsequently, you should run **notmuch new** whenever
new mail is delivered and you wish to incorporate it into the database.
These subsequent runs will be much quicker than the initial run.

Invoking ``notmuch`` with no command argument will run **new** if
**notmuch setup** has previously been completed, but **notmuch new** has
not previously been run.

**notmuch new** updates tags according to maildir flag changes if the
**maildir.synchronize\_flags** configuration option is enabled. See
**notmuch-config(1)** for details.

The **new** command supports hooks. See **notmuch-hooks(5)** for more
details on hooks.

Supported options for **new** include

    ``--no-hooks``
        Prevents hooks from being run.

    ``--quiet``
        Do not print progress or results.

    ``--decrypt=(true|nostash|auto|false)``

        If ``true``, when encountering an encrypted message, try to
        decrypt it while indexing, and stash any discovered session
        keys.  If ``auto``, try to use any session key already known
        to belong to this message, but do not attempt to use the
        user's secret keys.  If decryption is successful, index the
        cleartext of the message.

        Be aware that the index is likely sufficient (and the session
        key is certainly sufficient) to reconstruct the cleartext of
        the message itself, so please ensure that the notmuch message
        index is adequately protected.  DO NOT USE ``--decrypt=true``
        or ``--decrypt=nostash`` without considering the security of
        your index.

        See also ``index.decrypt`` in **notmuch-config(1)**.

EXIT STATUS
===========

This command supports the following special exit status code

``75 (EX_TEMPFAIL)``
    A temporary failure occurred; the user is invited to retry.

SEE ALSO
========

**notmuch(1)**,
**notmuch-config(1)**,
**notmuch-count(1)**,
**notmuch-dump(1)**,
**notmuch-hooks(5)**,
**notmuch-insert(1)**,
**notmuch-reply(1)**,
**notmuch-restore(1)**,
**notmuch-search(1)**,
**notmuch-search-terms(7)**,
**notmuch-show(1)**,
**notmuch-tag(1)**
