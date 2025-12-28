defmodule PmetricsTest.Application do
  use Application

  def start(_type, _args) do
    children = [
      PmetricsTest.Repo
    ]

    opts = [strategy: :one_for_one, name: PmetricsTest.Supervisor]
    Supervisor.start_link(children, opts)
  end
end
