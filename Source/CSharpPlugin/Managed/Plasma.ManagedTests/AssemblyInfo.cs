// The managed host is a process-wide singleton: Initialize/Shutdown, the generation table and the
// unmanaged extension tables are all static. Two test classes running at once would tear down each
// other's host mid-test, so the suite runs serially rather than pretending otherwise.
[assembly: CollectionBehavior(DisableTestParallelization = true)]
