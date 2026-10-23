"""Broker consumer boundary: acknowledge only after a successful target apply.

The caller owns the connection and must close it when this function raises.
RabbitMQ then requeues every delivery left unacknowledged on that channel.
"""
import time
from concurrent.futures import ThreadPoolExecutor, TimeoutError


def consume_after_apply(ch, queue, debounce_sec, reconcile_sec, apply_fn,
                        is_running, max_pending=100, maintain_connection=None):
    if max_pending < 1:
        raise ValueError("max_pending must be positive")
    ch.basic_qos(prefetch_count=max_pending)
    pending = []
    last_success = 0.0

    def apply_with_heartbeats():
        if maintain_connection is None:
            return apply_fn()
        # Pika stays on its owning thread. Only target work runs in the worker;
        # never overlap a retry with an apply still finishing after broker loss.
        broker_error = None
        with ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(apply_fn)
            while not future.done():
                if broker_error is None:
                    try:
                        maintain_connection()
                    except Exception as error:
                        broker_error = error
                try:
                    future.result(timeout=0.1)
                except TimeoutError:
                    pass
            result = future.result()
        if broker_error is not None:
            raise broker_error
        return result

    def flush():
        nonlocal last_success
        # The apply callback must explicitly report success. A false result or
        # exception leaves pending tags unacked; closing the channel requeues.
        if apply_with_heartbeats() is not True:
            raise RuntimeError("render target did not confirm successful apply")
        for tag in pending:
            ch.basic_ack(delivery_tag=tag)
        pending.clear()
        last_success = time.monotonic()

    # Reconcile before consuming even if the broker lost the triggering event.
    flush()
    for method, _props, _body in ch.consume(queue, inactivity_timeout=debounce_sec):
        if not is_running():
            break  # leave pending unacked for the next process/connection
        if method is not None:
            pending.append(method.delivery_tag)
        due = reconcile_sec > 0 and time.monotonic() - last_success >= reconcile_sec
        if pending and (method is None or len(pending) >= max_pending):
            flush()
        elif method is None and due:
            flush()
