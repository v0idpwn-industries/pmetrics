defmodule PmetricsTest.Repo do
  use Ecto.Repo,
    otp_app: :pmetrics_test,
    adapter: Ecto.Adapters.Postgres
end
