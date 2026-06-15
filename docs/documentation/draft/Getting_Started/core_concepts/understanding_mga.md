# Understanding Multi-Generational Architecture (MGA)

## Purpose

This page explains, in plain language, how ScratchBird keeps your data safe while many people read and write at the same time. The mechanism is called **Multi-Generational Architecture**, or **MGA**. You do not need to be a programmer to follow it; the analogies below build the intuition first, and the later sections connect that intuition to the real engine behavior.

MGA is the reason ScratchBird can let one person read a stable view of the data while another person is busy changing it, without either one waiting on the other. It is also the foundation for features described elsewhere in this guide, such as snapshot isolation, point-in-time history, and safe background cleanup.

For the architectural treatment of the same topic, see [Storage, Transactions, And Recovery](../architecture/storage_transactions_and_recovery.md). For the language-level transaction contract, see the [Transaction Control](../../Language_Reference/syntax_reference/transaction_control.md) page in the Language Reference.


### 1. Always in a Transaction (The Safety Bubble)

Whenever you open the database, you are automatically placed inside a **Transaction**. Think of this as your own private safety bubble. Anything you do inside this bubble isn't real to the rest of the world until you hit "Save" (Commit). If something goes wrong, the bubble pops (Rolls back), and it's like it never happened.

### 2. Always in Snapshot Mode (The Time-Freeze Photo)

The moment your safety bubble opens, the database takes a digital snapshot of the entire system.

- You only see data that was officially saved **before** your snapshot was taken.

- Even if another user changes a row while you are working, your view remains frozen in time. You will never experience data shifting under your feet.

### 3. Non-Destructive Changes (The LiveJournal Page)

When you **Delete** or **Update** a row, the database *never* overwrites or destroys the original data on the disk.

- **An Update:** Writes a brand new version of the row right next to the old one, with a note saying: *"If you want the version before this, look over there."*

- **A Delete:** Doesn't erase anything. It just attaches a "Dead" sticky note to the row.

## Visualizing the LiveJournal Page (Record History)

This diagram shows how different users see the exact same piece of data depending on when their "Snapshot Photo" was taken.

![diagram](./understanding_mga-1.svg)

## How the Database Decides What You See

When a non-programmer reads the "LiveJournal" page, the database acts as a smart filter by asking three simple questions:

1. **Is the latest version officially saved?** * Looking at the diagram, **Tom's Version 3** isn't saved yet (it's Active). The database immediately ignores it for other users.

2. **Was it saved *before* my snapshot photo was taken?**
   
   - **User A** took their photo after Sarah saved. They see **Version 2 ($12.00)**.
   
   - **User B** took their photo a long time ago, before Sarah even started writing. The database rolls them all the way back to **Version 1 ($10.00)**.

3. **What happens to the old stuff?**
   
   Instead of blindly erasing old data to save space (which can corrupt active users' views), ScratchBird uses a careful, two-stage system: **Logical Cleanup** (deciding what is safe to delete) and **Physical Cleanup** (actually reclaiming the disk space).

It functions like a team of editors managing a shared historical journal. They won't delete an old draft until they are 100% sure no reader in the building is still looking at it.

### 1. Dealing with Long-Running Users (Transaction Pressure)

Because every user session is *always* inside a frozen "snapshot" transaction, a single user who leaves their connection open can block the system from cleaning up old data. ScratchBird manages this with a strict escalation policy.

#### The "Do Not Disturb" Boundary (The Horizon)

ScratchBird calculates a global **Cleanup Horizon**. This is the exact moment in time behind which data is safe to destroy.

- It checks active snapshots, unresolved data, and open sessions to find the absolute **oldest thing anyone cares about**.

- Anything newer than this horizon is strictly hands-off.

#### The Escalation Timer (Pressure Management)

If a user is idle but their open session is keeping the horizon from moving forward, a background manager starts a countdown clock to nudge them out of the way:

| **Time Elapsed** | **ScratchBird's Action**                                                                                                                       |
| ---------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| **~5 Minutes**   | **Warn:** Sends a gentle warning to the client application.                                                                                    |
| **~15 Minutes**  | **Request Restart:** Asks the application to recycle its transaction.                                                                          |
| **~20 Minutes**  | **Request Reauth:** Asks the user to re-authenticate.                                                                                          |
| **~25 Minutes**  | **Request Cancel:** Asks to gracefully cancel the blocking work.                                                                               |
| **~30 Minutes**  | **Force Replacement:** If allowed by policy, it forcibly replaces the old transaction boundary with a fresh one to let cleanup proceed safely. |

### 2. Dealing with Clutter (Page & Index Bloat)

ScratchBird handles old data versions incrementally in a series of careful checkpoints, ensuring background cleanup never slows down the user's active work.

#### Step 1: Logical Version Cleanup

The engine groups old row versions into a "candidate list." A row version is only eligible to be destroyed if:

1. It was explicitly deleted, rolled back, or replaced by a newer saved version.

2. The transaction that created it is older than the **Cleanup Horizon**.

3. It passes a rigorous safety check ensuring it isn't blocked by an active backup, a legal archive hold, or a recovery process.

#### Step 2: Physical Page Compaction

Once a row version is logically cleared, the physical cleanup coordinator steps in with the "reclaim evidence." It shrinks and packs the actual database pages, recycling the empty space. It refuses to do this unless it has airtight proof from Step 1.

#### Step 3: Index Maintenance

Indexes (the data phonebooks) are cleaned up just as carefully. ScratchBird checks the actual table snapshot before removing stale index entries from its ledger, ensuring search shortcuts never break.

#### Step 4: Debt Scheduling (The Budget)

To keep the database fast, cleanup is treated as a continuous, low-priority background task.

- It scores "cleanup debt" (where the most clutter is).

- If the user is doing heavy foreground work, the cleanup budget is automatically scaled back so it never starves the system of performance.

### The Ultimate Rule of ScratchBird

> **Safety Over Space:** If a long-running transaction or legal snapshot remains active and authorized, ScratchBird will **always** choose to retain the old versions and let the database grow rather than risk data corruption. It will pressure the blocker to move, but it will never compromise visibility rules.

## How Do Other Database Engines Do This?

Most database systems use WAL - Write Ahead Logs, which is designed with a very different approach to operations.
To understand the difference between these two systems without getting bogged down in computer science jargon, imagine you run a busy **Legal Archive Office** where team members are constantly changing and reading documents.

**Standard WAL** and **ScratchBird MGA** represent two completely different strategies for running this office so that files never get lost and workers don't trip over each other.

## 1. Standard WAL: The "Notebook and Eraser" Strategy

In a standard WAL (Write-Ahead Logging) office, there is one main master binder on the shelf, and one sequential logbook on the desk.

- **Making a Change:** When a worker wants to change a price from \$10$ \ to \  $\$12, they must write it in the logbook first: *"Entry 45: Changing page 5 from \$10$ \ to\   $\$12."* 

- Only after it is written in the logbook are they allowed to go to the master binder, take an eraser, **scratch out \$10$,\  and\  overwrite\  it\  with\  $\$12**.

- **Changing Your Mind (Cancel):** If a worker gets halfway through a massive change and decides to cancel, it is a lot of work. They have to read the logbook backward and physically erase their mistakes in the master binder to restore the original numbers.

- **The Traffic Jam:** Because there is only one master copy of the page, if someone is in the middle of erasing and overwriting a line, nobody else is allowed to look at that page. Readers have to wait in line.

## 2. ScratchBird MGA: The "LiveJournal" Strategy

ScratchBird throws away the erasers. The binders act like a chronological journal where **nothing is ever crossed out or destroyed**.

- **Making a Change:** When a worker wants to change a price from \$10$\  to\  $\$12, they leave the original line completely alone. They simply write a brand new line underneath it: *Price is \$12 (Written by Tom).* Then, they draw a little arrow pointing back to the old line, creating a historical chain.

- **Changing Your Mind (Cancel):** Canceling is instant. If Tom decides to cancel his work, he doesn't have to erase anything. The office manager simply flips a switch marking Tom's name as "Invalid." The next person who reads the book sees Tom's line, sees his name is invalid, and completely ignores it.

- **No Traffic Jams:** Because old data is never destroyed, a reader can open a binder and read a frozen snapshot of the past while a writer is actively adding new lines at the bottom of the same page. No one ever has to wait in line to read.

## 3. How They Handle a Crisis

The biggest danger in a busy office is a worker who opens a book, starts a project, and then goes to lunch for three hours without closing it. Here is how both systems handle that crisis:

### The WAL Crisis: The Logbook Explodes

Because the master binder only shows the *current* moment, the office cannot throw away or archive the desk logbook while a project is open—it might still need those logs to fix a mistake.

- **The Problem:** As other workers keep doing business, the desk logbook grows longer and longer, eventually spilling out of the room and consuming the entire building until there is no physical space left to stand.

- **The Result:** The office completely grinds to a halt.

### The ScratchBird Crisis: The Binders Get Heavy

Because ScratchBird keeps historical lines on the page, if a worker keeps a project open for three hours, the office cannot clean up any old lines written during those three hours, because that worker might still look at them.

- **The Problem:** The pages get cluttered with old versions, and the binders get thick and heavy (page bloat).

- **The ScratchBird Solution:** ScratchBird has a built-in "Pressure Manager" that acts like an attentive office supervisor. If it sees a worker has gone to lunch and left a project open, a timer starts:
  
  1. **5 Mins:** The supervisor gives them a gentle nudge.
  
  2. **15 Mins:** Asks them to wrap it up and restart.
  
  3. **25 Mins:** Asks them to cancel.
  
  4. **30 Mins:** If allowed, the supervisor safely takes their paperwork, closes their frozen project, and opens a fresh one for them. This unlocks the backlog and allows the office cleaning crew to shred the unneeded historical lines, shrinking the binders back down.

## Quick Summary

- **Standard WAL** is like a traditional ledger. It's great for quick, short changes, but it relies on erasing data in-place and can cause lines to form when the office gets busy.

- **ScratchBird MGA** is like a historical diary. It takes a little more paper because it keeps a history of changes directly on the page, but it ensures that nobody ever blocks anyone else from reading, and it has a smart supervisor to keep the clutter under control.

## Beyond Simple MGA

## 1. No More "Spy Cameras" (Eliminating CDC Records)

In a traditional database, if you want to copy data to another system or run complex reports (ETL/Recursion), you have to install a "spy camera" called CDC (Change Data Capture). This camera watches the database constantly, writing down every single move a user makes into a separate, heavy stack of paperwork just so you can replay it later.

### How ScratchBird Does It: The "Whodunit" Journal

Because ScratchBird’s LiveJournal page *already* preserves every version of a row with the writer’s name (Transaction ID) stamped on it, you don't need a spy camera.

- The database record itself tells you exactly who has touched it since the last time you checked.

- Instead of sorting through a giant separate mountain of spy logs, a reporting tool can just look directly at the page and say: *"Show me lines written by anyone who arrived after Transaction 500."* ---

## 2. The Database Time Machine (Point-in-Time Queries)

Normally, as we discussed, the office cleaning crew (Garbage Collection) shreds old lines once everyone goes home, to keep the binders from getting too thick. But what if you need to know exactly what the database looked like last Tuesday at 3:00 PM?

### How ScratchBird Does It: The Archive Room

Instead of throwing old row versions into a paper shredder, ScratchBird's cleaning crew safely packs those old historical lines into **Archive Storage Binders** (Archive Filespaces).


![diagram](./understanding_mga-2.svg)

When you ask ScratchBird to travel back in time:

1. You give it a date and time (e.g., *Last Tuesday at 3:00 PM*).

2. The database manager flips through its master schedule and figures out exactly which transaction bubble was active at that exact minute.

3. It opens up the main binders *and* the archive binders, ignores everything written after that deadline, and lets you see the world exactly as it looked at that moment.

## 3. DNA Tracking Across the Whole Building (Cluster-Wide UUIDs)

In a giant enterprise system, data isn't kept in just one binder; it’s spread across a **Cluster** of different computers (like an archive office with 24 different rooms).

In a standard setup, if a row gets copied or moved to another room, it gets a new local line number, and its history is lost. It's like a person changing their identity every time they cross state lines.

### How ScratchBird Does It: The Unchangeable Passport (UUIDs)

ScratchBird gives every single piece of data its own universal, lifelong fingerprint: a **UUID**.

- No matter which computer room a record moves to, splits into, or clones into across the entire global cluster, its UUID passport stays exactly the same.

- Because the historical journal tracks changes by UUID rather than by local page numbers, ScratchBird can follow the full family tree and history of a single record across the entire network of computers seamlessly.

## Summary of the "Next-Level" Upgrade

By anchoring everything to permanent **UUID fingerprints** and moving old data to **Archive Filespaces** instead of destroying it, ScratchBird transforms the database from a simple storage box into a living history book. It gives you all the benefits of data tracking and time travel automatically, without slowing down the daily business of the office.
