from praktika import Workflow

from ci.workflows.defs import ARTIFACTS, BASE_BRANCH, DOCKERS, SECRETS, Jobs

workflow = Workflow.Config(
    name="PR",
    event=Workflow.Event.PULL_REQUEST,
    base_branches=[BASE_BRANCH],
    jobs=[
        Jobs.style_check_job,
        Jobs.fast_test_job,
        *Jobs.build_jobs,
        *Jobs.stateless_tests_jobs,
        *Jobs.stateful_tests_jobs,
        *Jobs.stress_test_jobs,
    ],
    artifacts=ARTIFACTS,
    dockers=DOCKERS,
    secrets=SECRETS,
    enable_cache=True,
    enable_report=True,
    enable_merge_ready_status=True,
)

WORKFLOWS = [
    workflow,
]


# if __name__ == "__main__":
#     # local job test inside praktika environment
#     from praktika.runner import Runner
#     from praktika.digest import Digest
#
#     print(Digest().calc_job_digest(amd_debug_build_job))
#
#     Runner().run(workflow, fast_test_job, docker="fasttest", local_run=True)
