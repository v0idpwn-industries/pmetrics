defmodule PmetricsTest.MixProject do
  use Mix.Project

  def project do
    [
      app: :pmetrics_test,
      version: "0.1.0",
      elixir: "~> 1.14",
      start_permanent: false,
      deps: deps()
    ]
  end

  def application do
    [
      extra_applications: [:logger],
      mod: {PmetricsTest.Application, []}
    ]
  end

  defp deps do
    [
      {:ecto_sql, "~> 3.10"},
      {:postgrex, "~> 0.17"},
      {:jason, "~> 1.4"}
    ]
  end
end
