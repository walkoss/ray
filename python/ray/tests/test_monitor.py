import sys

import pytest

import ray
import ray._private.gcs_utils as gcs_utils
from ray.autoscaler._private.monitor import parse_resource_demands

ray.experimental.internal_kv.redis = False


def test_parse_resource_demands():
    resource_load_by_shape = gcs_utils.ResourceLoad(
        resource_demands=[
            gcs_utils.ResourceDemand(
                shape={"CPU": 1},
                num_ready_requests_queued=1,
                num_infeasible_requests_queued=0,
                backlog_size=0,
            ),
            gcs_utils.ResourceDemand(
                shape={"CPU": 2},
                num_ready_requests_queued=1,
                num_infeasible_requests_queued=0,
                backlog_size=1,
            ),
            gcs_utils.ResourceDemand(
                shape={"CPU": 3},
                num_ready_requests_queued=0,
                num_infeasible_requests_queued=1,
                backlog_size=2,
            ),
            gcs_utils.ResourceDemand(
                shape={"CPU": 4},
                num_ready_requests_queued=1,
                num_infeasible_requests_queued=1,
                backlog_size=2,
            ),
        ]
    )

    waiting, infeasible = parse_resource_demands(resource_load_by_shape)

    assert waiting.count({"CPU": 1}) == 1
    assert waiting.count({"CPU": 2}) == 2
    assert infeasible.count({"CPU": 3}) == 3
    # The {"CPU": 4} case here is inconsistent, but could happen. Since the
    # heartbeats are eventually consistent, we won't worry about whether it's
    # counted as infeasible or waiting, as long as it's accounted for and
    # doesn't cause an error.
    assert len(waiting + infeasible) == 10


if __name__ == "__main__":
    sys.exit(pytest.main(["-sv", __file__]))
